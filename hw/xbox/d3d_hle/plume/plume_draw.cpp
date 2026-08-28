/*
 * plume_draw.cpp — Draw replay, PSO cache, textures, programmable xps path.
 */
#include "plume_draw.h"
#include "plume_debug_overlay.h"
#include "plume_surface_download.h"
#include "plume_buffer_upload.h"
#include "plume_context.h"
#include "plume_f2_capture.h"
#include "plume_frame_constants.h"
#include "plume_pipeline_layout.h"
#include "plume_render_state.h"
#include "plume_resolution_scale.h"
#include "plume_shader_compiler.h"
#include "plume_fixed_function.h"
#include "plume_output_quad.h"
#include "plume_texture_state.h"
#include "plume_ui_canvas.h"
#include "plume_vertex_stream.h"
#include "xps_translate.h"
#include "../d3d8_swizzle.h"
#include "../d3d8_vsh.h"
#include "../kernel/xbox_memory_layout.h"
/* NV2A pgraph YUV oracle: BT.601 integer conversion shared with xemu's
 * native texture path (convert_yuy2_to_rgb / convert_uyvy_to_rgb). */
#include "../../nv2a/pgraph/util.h"

#include "../platform/host_time.h"
#include "../platform/cpu_recorder.h"
#include "../platform/xrecomp_tracy.h"
#include "plume_perf.h"
#include "plume_wait_stats.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <type_traits>

namespace xgpu {
namespace plume {

using namespace ::plume;

namespace {

constexpr uint32_t kHostFrameGuest = 0xFFFFFFFEu;
constexpr uint32_t kHostFrameFormat = 0xFFFFFFFFu;
/* Four dynamic samplers per set consume Plume's 2048-entry D3D12 sampler
 * heap (the API maximum), and program sets are its only dynamic-sampler
 * consumers — every other layout binds immutable samplers, which Plume
 * bakes outside this heap. Under the pipelined present two frames of sets
 * are alive at once (frame N retires only after its present fence), so the
 * budget is 2 x limit x 4 <= 2048 with headroom: 2x240x4 = 1920. At the old
 * limit of 48 the mid-replay flush — a full total-order GPU drain — fired
 * on every other present (0.43 ms/present measured); a typical heavy frame
 * uses 48-96 unique sets, so 240 makes the valve an actual last resort. */
constexpr size_t kProgDescriptorBatchLimit = 240u;
constexpr uint64_t kBackbufferMirrorBit = 1ull << 63;

struct ProgrammableUiCanvasTransform {
    PlumeUiCanvasHorizontalTransform horizontal;
};

ProgrammableUiCanvasTransform programmableUiCanvasTransform(
    const XgpuPlumeRenderState &state,
    uint32_t targetWidth,
    uint32_t targetHeight)
{
    ProgrammableUiCanvasTransform transform;

    if (!state.ui_canvas_active
        || state.ui_canvas_width <= 0.0f
        || state.ui_canvas_height <= 0.0f
        || targetWidth == 0u
        || std::fabs(static_cast<float>(targetHeight)
                     - state.ui_canvas_height) > 1.0f) {
        return transform;
    }
    if (state.ui_canvas_mode == XGPU_PLUME_UI_CANVAS_CENTERED) {
        plumeComputeUiCanvasHorizontalTransform(
            0.0f, state.ui_canvas_width,
            state.ui_canvas_width, static_cast<float>(targetWidth),
            0.0f, state.ui_canvas_width, false, true,
            transform.horizontal);
    } else if (state.programmable_position_bounds_valid) {
        plumeComputeUiCanvasHorizontalTransform(
            state.programmable_position_min_x,
            state.programmable_position_max_x,
            state.ui_canvas_width, static_cast<float>(targetWidth),
            0.0f, state.ui_canvas_width, false, false,
            transform.horizontal);
    } else {
        /* If a frontend cannot provide transformed draw bounds, preserve the
         * complete authored canvas rather than classifying separate vertices
         * into different regions and tearing a widget. */
        plumeComputeUiCanvasHorizontalTransform(
            0.0f, state.ui_canvas_width,
            state.ui_canvas_width, static_cast<float>(targetWidth),
            0.0f, state.ui_canvas_width, false, true,
            transform.horizontal);
    }
    return transform;
}

RenderRect drawScissorRect(const XgpuPlumeRenderState &state,
                           uint32_t targetWidth,
                           uint32_t targetHeight,
                           uint32_t scale)
{
    if (!state.scissor_enable)
        return RenderRect(0, 0, targetWidth * scale, targetHeight * scale);

    const uint32_t left = std::min(state.scissor_x, targetWidth);
    const uint32_t top = std::min(state.scissor_y, targetHeight);
    const uint32_t right = static_cast<uint32_t>(std::min<uint64_t>(
        static_cast<uint64_t>(state.scissor_x) + state.scissor_width,
        targetWidth));
    const uint32_t bottom = static_cast<uint32_t>(std::min<uint64_t>(
        static_cast<uint64_t>(state.scissor_y) + state.scissor_height,
        targetHeight));
    return RenderRect(left * scale, top * scale,
                      right * scale, bottom * scale);
}

constexpr uint64_t backbufferMirrorGeneration(uint32_t guest)
{
    return kBackbufferMirrorBit | guest;
}

constexpr bool isBackbufferMirrorGeneration(uint64_t generation,
                                            uint32_t guest)
{
    return guest && generation == backbufferMirrorGeneration(guest);
}

const char *live_shader_override_dir()
{
    static const char *directory = []() -> const char * {
        const char *value =
            std::getenv("XRECOMP_PLUME_SHADER_OVERRIDE_DIR");
        if (!value || !*value)
            return nullptr;
        std::error_code error;
        std::filesystem::create_directories(value, error);
        if (error) {
            std::fprintf(stderr,
                         "[PLUME-LIVE] unable to create shader directory "
                         "'%s': %s\n",
                         value, error.message().c_str());
            return nullptr;
        }
        std::fprintf(stderr, "[PLUME-LIVE] shader overrides: %s\n", value);
        return value;
    }();
    return directory;
}

int64_t live_shader_write_stamp(const std::string &path)
{
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<int64_t>(
        stamp.time_since_epoch().count());
}

bool seed_live_pixel_shader(uint32_t handle, const std::string &hlsl,
                            std::string &path, int64_t &stamp)
{
    const char *directory = live_shader_override_dir();
    if (!directory || !handle || hlsl.empty())
        return false;
    char name[48];
    std::snprintf(name, sizeof(name), "plume_live_ps_%08X.hlsl", handle);
    path = (std::filesystem::path(directory) / name).string();
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::fprintf(stderr,
                     "[PLUME-LIVE] unable to seed pixel shader %08X at %s\n",
                     handle, path.c_str());
        path.clear();
        stamp = 0;
        return false;
    }
    file.write(hlsl.data(), static_cast<std::streamsize>(hlsl.size()));
    file.close();
    stamp = live_shader_write_stamp(path);
    return stamp != 0;
}

bool read_live_shader(const std::string &path, std::string &text)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    text.assign(std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>());
    return !text.empty();
}

ShaderCompileResult compileForTarget(ShaderTarget target, const char *source,
                                     const char *entryPoint,
                                     const char *profile)
{
    ShaderCompileRequest request;
    request.source = source;
    request.entryPoint = entryPoint;
    request.profile = profile;
    request.target = target;

    const uint64_t perfShaderT0 = xgpu_plume_perf_begin();
    ShaderCompileResult result = compileShader(request);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_SHADER, perfShaderT0);
    if (!result.ok) {
        std::fprintf(stderr, "[PLUME] shader compile failed: %s\n",
                     result.diagnostics.c_str());
    }
    return result;
}

ShaderCompileResult compileForContext(PlumeContext &ctx, const char *source,
                                      const char *entryPoint,
                                      const char *profile)
{
    return compileForTarget(
        shaderTargetForRenderFormat(
            ctx.iface()->getCapabilities().shaderFormat),
        source, entryPoint, profile);
}

std::future<ShaderCompileResult> queueShaderCompile(
    ShaderTarget target, const std::string &source, const char *profile)
{
    ShaderCompileRequest request;
    request.source = source.c_str();
    request.entryPoint = "main";
    request.profile = profile;
    request.target = target;
    return compileShaderAsync(request);
}

bool shaderCompileReady(std::future<ShaderCompileResult> &future)
{
    return future.valid() &&
           future.wait_for(std::chrono::seconds(0)) ==
               std::future_status::ready;
}

void f2_dump_linear_r5g6b5(uint32_t guest, const void *pixels,
                           uint32_t width, uint32_t height, uint32_t pitch,
                           uint32_t bytes, uint64_t version)
{
    static std::unordered_map<uint32_t, uint64_t> dumped;
    char path[96];
    FILE *file;
    const uint8_t *source = static_cast<const uint8_t *>(pixels);
    std::vector<uint8_t> row;

    if (!xgpu_plume_f2_active() || !guest || !pixels || !width || !height)
        return;
    if (!pitch)
        pitch = width * 2u;
    if (pitch < width * 2u ||
        static_cast<uint64_t>(pitch) * height > bytes)
        return;
    auto previous = dumped.find(guest);
    if (previous != dumped.end() && previous->second == version)
        return;
    dumped[guest] = version;

    std::snprintf(path, sizeof(path),
                  "plume_f2_tex_%08X_v%016llX.ppm", guest,
                  static_cast<unsigned long long>(version));
    file = std::fopen(path, "wb");
    if (!file) {
        xgpu_plume_f2_log("texdump failed guest=%08X path=%s", guest, path);
        return;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
    row.resize(static_cast<size_t>(width) * 3u);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *sourceRow =
            source + static_cast<size_t>(y) * pitch;
        for (uint32_t x = 0; x < width; ++x) {
            const uint16_t pixel = static_cast<uint16_t>(
                sourceRow[x * 2u] |
                (static_cast<uint16_t>(sourceRow[x * 2u + 1u]) << 8));
            const uint8_t r5 = static_cast<uint8_t>((pixel >> 11) & 0x1Fu);
            const uint8_t g6 = static_cast<uint8_t>((pixel >> 5) & 0x3Fu);
            const uint8_t b5 = static_cast<uint8_t>(pixel & 0x1Fu);
            row[x * 3u] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
            row[x * 3u + 1u] =
                static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
            row[x * 3u + 2u] =
                static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    xgpu_plume_f2_log(
        "texdump guest=%08X version=%016llX format=11 size=%ux%u "
        "pitch=%u path=%s",
        guest, static_cast<unsigned long long>(version), width, height,
        pitch, path);
}

void f2_dump_linear_bgra8(uint32_t guest, const void *pixels,
                          uint32_t width, uint32_t height, uint32_t pitch,
                          uint32_t bytes, uint32_t format, uint64_t version)
{
    static std::unordered_map<uint32_t, uint64_t> dumped;
    static uint32_t dumpCount;
    constexpr uint32_t kDumpLimit = 64u;
    char rgbPath[112];
    char alphaPath[112];
    FILE *rgbFile;
    FILE *alphaFile;
    const uint8_t *source = static_cast<const uint8_t *>(pixels);
    std::vector<uint8_t> rgbRow;
    std::vector<uint8_t> alphaRow;

    if (!xgpu_plume_f2_active() || !guest || !pixels || !width || !height ||
        (format != 0x06u && format != 0x12u && format != 0x1Eu))
        return;
    if (!pitch)
        pitch = width * 4u;
    if (pitch < width * 4u ||
        static_cast<uint64_t>(pitch) * height > bytes)
        return;
    auto previous = dumped.find(guest);
    if (previous != dumped.end() && previous->second == version)
        return;
    if (dumpCount >= kDumpLimit)
        return;
    dumped[guest] = version;
    ++dumpCount;

    std::snprintf(rgbPath, sizeof(rgbPath),
                  "plume_f2_tex_%08X_v%016llX_f%02X.ppm", guest,
                  static_cast<unsigned long long>(version), format);
    std::snprintf(alphaPath, sizeof(alphaPath),
                  "plume_f2_tex_%08X_v%016llX_f%02X_a.pgm", guest,
                  static_cast<unsigned long long>(version), format);
    rgbFile = std::fopen(rgbPath, "wb");
    alphaFile = std::fopen(alphaPath, "wb");
    if (!rgbFile || !alphaFile) {
        if (rgbFile)
            std::fclose(rgbFile);
        if (alphaFile)
            std::fclose(alphaFile);
        xgpu_plume_f2_log(
            "texdump failed guest=%08X rgb=%s alpha=%s",
            guest, rgbPath, alphaPath);
        return;
    }
    std::fprintf(rgbFile, "P6\n%u %u\n255\n", width, height);
    std::fprintf(alphaFile, "P5\n%u %u\n255\n", width, height);
    rgbRow.resize(static_cast<size_t>(width) * 3u);
    alphaRow.resize(width);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *sourceRow =
            source + static_cast<size_t>(y) * pitch;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t *pixel = sourceRow + x * 4u;
            rgbRow[x * 3u] = pixel[2];
            rgbRow[x * 3u + 1u] = pixel[1];
            rgbRow[x * 3u + 2u] = pixel[0];
            alphaRow[x] = format == 0x1Eu ? 255u : pixel[3];
        }
        std::fwrite(rgbRow.data(), 1, rgbRow.size(), rgbFile);
        std::fwrite(alphaRow.data(), 1, alphaRow.size(), alphaFile);
    }
    std::fclose(rgbFile);
    std::fclose(alphaFile);
    xgpu_plume_f2_log(
        "texdump guest=%08X version=%016llX format=%02X size=%ux%u "
        "pitch=%u rgb=%s alpha=%s",
        guest, static_cast<unsigned long long>(version), format,
        width, height, pitch, rgbPath, alphaPath);
}

void f2_dump_vertex_shader(uint32_t handle, const std::string &hlsl)
{
    static std::unordered_map<uint32_t, bool> dumped;
    char path[64];
    FILE *file;

    if (!xgpu_plume_f2_active() || !handle || hlsl.empty() ||
        dumped.find(handle) != dumped.end())
        return;
    dumped[handle] = true;
    std::snprintf(path, sizeof(path), "plume_f2_vsh_%08X.hlsl", handle);
    file = std::fopen(path, "wb");
    if (!file) {
        xgpu_plume_f2_log("vshdump failed handle=%08X path=%s", handle, path);
        return;
    }
    std::fwrite(hlsl.data(), 1, hlsl.size(), file);
    std::fclose(file);
    xgpu_plume_f2_log("vshdump handle=%08X bytes=%zu path=%s",
                      handle, hlsl.size(), path);
}

void f2_dump_pixel_shader(uint32_t handle, const std::string &hlsl)
{
    static std::unordered_map<uint32_t, bool> dumped;
    char path[64];
    FILE *file;

    if (!xgpu_plume_f2_active() || !handle || hlsl.empty() ||
        dumped.find(handle) != dumped.end())
        return;
    dumped[handle] = true;
    std::snprintf(path, sizeof(path), "plume_f2_ps_%08X.hlsl", handle);
    file = std::fopen(path, "wb");
    if (!file) {
        xgpu_plume_f2_log("psdump failed handle=%08X path=%s", handle, path);
        return;
    }
    std::fwrite(hlsl.data(), 1, hlsl.size(), file);
    std::fclose(file);
    xgpu_plume_f2_log("psdump handle=%08X bytes=%zu path=%s",
                      handle, hlsl.size(), path);
}

} /* namespace */

static ::plume::RenderFormat nv2a_attr_render_format(uint32_t format);
static uint32_t nv2a_attr_host_size(uint32_t format);

static void plume_upload_layout(RenderFormat pf, uint32_t widthPx,
                                uint32_t *outRowWidthPx, uint32_t *outRowPitch)
{
    const uint32_t blockW = RenderFormatBlockWidth(pf);
    const uint32_t fmtSize = RenderFormatSize(pf);
    uint32_t rowWidth = widthPx;
    uint32_t rowPitch = ((rowWidth + blockW - 1u) / blockW) * fmtSize;
    /* D3D12 requires RowPitch % 256 == 0; widen rowWidth in block steps. */
    while ((rowPitch % 256u) != 0u) {
        rowWidth += blockW;
        rowPitch = ((rowWidth + blockW - 1u) / blockW) * fmtSize;
    }
    *outRowWidthPx = rowWidth;
    *outRowPitch = rowPitch;
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void bc3_color(uint16_t c, uint8_t rgba[4])
{
    const uint8_t r = (uint8_t)((c >> 11) & 31u);
    const uint8_t g = (uint8_t)((c >> 5) & 63u);
    const uint8_t b = (uint8_t)(c & 31u);
    rgba[0] = (uint8_t)((r << 3) | (r >> 2));
    rgba[1] = (uint8_t)((g << 2) | (g >> 4));
    rgba[2] = (uint8_t)((b << 3) | (b >> 2));
    rgba[3] = 255;
}

static void decode_bc3_block(const uint8_t *block, uint8_t rgba[16][4])
{
    uint8_t alpha[8];
    uint8_t color[4][4];
    uint64_t alphaBits = 0;
    uint32_t colorBits;

    alpha[0] = block[0];
    alpha[1] = block[1];
    if (alpha[0] > alpha[1]) {
        for (uint32_t i = 1; i <= 6; i++)
            alpha[i + 1] = (uint8_t)(((7u - i) * alpha[0] + i * alpha[1]) / 7u);
    } else {
        for (uint32_t i = 1; i <= 4; i++)
            alpha[i + 1] = (uint8_t)(((5u - i) * alpha[0] + i * alpha[1]) / 5u);
        alpha[6] = 0;
        alpha[7] = 255;
    }
    for (uint32_t i = 0; i < 6; i++)
        alphaBits |= (uint64_t)block[2 + i] << (i * 8);

    bc3_color(read_le16(block + 8), color[0]);
    bc3_color(read_le16(block + 10), color[1]);
    for (uint32_t channel = 0; channel < 3; channel++) {
        color[2][channel] = (uint8_t)((2u * color[0][channel] + color[1][channel]) / 3u);
        color[3][channel] = (uint8_t)((color[0][channel] + 2u * color[1][channel]) / 3u);
    }
    color[2][3] = color[3][3] = 255;
    colorBits = (uint32_t)block[12] | ((uint32_t)block[13] << 8) |
                ((uint32_t)block[14] << 16) | ((uint32_t)block[15] << 24);
    for (uint32_t pixel = 0; pixel < 16; pixel++) {
        const uint32_t ci = (colorBits >> (pixel * 2)) & 3u;
        const uint32_t ai = (uint32_t)((alphaBits >> (pixel * 3)) & 7u);
        std::memcpy(rgba[pixel], color[ci], 4);
        rgba[pixel][3] = alpha[ai];
    }
}

static void decode_bc1_block(const uint8_t *block, uint8_t rgba[16][4])
{
    uint8_t color[4][4];
    uint32_t colorBits;
    const uint16_t c0 = read_le16(block);
    const uint16_t c1 = read_le16(block + 2);

    bc3_color(c0, color[0]);
    bc3_color(c1, color[1]);
    if (c0 > c1) {
        for (uint32_t channel = 0; channel < 3; ++channel) {
            color[2][channel] = (uint8_t)(
                (2u * color[0][channel] + color[1][channel]) / 3u);
            color[3][channel] = (uint8_t)(
                (color[0][channel] + 2u * color[1][channel]) / 3u);
        }
        color[2][3] = color[3][3] = 255;
    } else {
        for (uint32_t channel = 0; channel < 3; ++channel)
            color[2][channel] =
                (uint8_t)((color[0][channel] + color[1][channel]) / 2u);
        color[2][3] = 255;
        std::memset(color[3], 0, sizeof(color[3]));
    }

    colorBits = (uint32_t)block[4] | ((uint32_t)block[5] << 8) |
                ((uint32_t)block[6] << 16) | ((uint32_t)block[7] << 24);
    for (uint32_t pixel = 0; pixel < 16; ++pixel)
        std::memcpy(rgba[pixel],
                    color[(colorBits >> (pixel * 2u)) & 3u], 4);
}

static void f2_dump_dxt_texture(uint32_t guest, const void *pixels,
                                uint32_t width, uint32_t height,
                                uint32_t bytes, uint32_t format,
                                uint64_t version)
{
    static std::unordered_map<uint32_t, uint64_t> dumped;
    static uint32_t dumpCount;
    constexpr uint32_t kDumpLimit = 192u;
    const uint32_t blockBytes = format == 0x0Cu ? 8u : 16u;
    const uint32_t blocksX = (width + 3u) / 4u;
    const uint32_t blocksY = (height + 3u) / 4u;
    const uint64_t required =
        static_cast<uint64_t>(blocksX) * blocksY * blockBytes;
    const uint8_t *source = static_cast<const uint8_t *>(pixels);
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3u);
    std::vector<uint8_t> alpha(static_cast<size_t>(width) * height);
    char rgbPath[112];
    char alphaPath[112];

    if (!xgpu_plume_f2_active() || !guest || !pixels || !width || !height ||
        (format != 0x0Cu && format != 0x0Fu) || required > bytes)
        return;
    auto previous = dumped.find(guest);
    if (previous != dumped.end() && previous->second == version)
        return;
    if (dumpCount >= kDumpLimit)
        return;
    dumped[guest] = version;
    ++dumpCount;

    for (uint32_t by = 0; by < blocksY; ++by) {
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            uint8_t rgba[16][4];
            const uint8_t *block =
                source + (static_cast<size_t>(by) * blocksX + bx) *
                             blockBytes;
            if (format == 0x0Cu)
                decode_bc1_block(block, rgba);
            else
                decode_bc3_block(block, rgba);
            for (uint32_t py = 0; py < 4u; ++py) {
                const uint32_t y = by * 4u + py;
                if (y >= height)
                    continue;
                for (uint32_t px = 0; px < 4u; ++px) {
                    const uint32_t x = bx * 4u + px;
                    if (x >= width)
                        continue;
                    const uint8_t *decoded = rgba[py * 4u + px];
                    const size_t pixel = static_cast<size_t>(y) * width + x;
                    std::memcpy(rgb.data() + pixel * 3u, decoded, 3u);
                    alpha[pixel] = decoded[3];
                }
            }
        }
    }

    std::snprintf(rgbPath, sizeof(rgbPath),
                  "plume_f2_tex_%08X_v%016llX_f%02X.ppm", guest,
                  static_cast<unsigned long long>(version), format);
    std::snprintf(alphaPath, sizeof(alphaPath),
                  "plume_f2_tex_%08X_v%016llX_f%02X_a.pgm", guest,
                  static_cast<unsigned long long>(version), format);
    FILE *rgbFile = std::fopen(rgbPath, "wb");
    FILE *alphaFile = std::fopen(alphaPath, "wb");
    if (!rgbFile || !alphaFile) {
        if (rgbFile)
            std::fclose(rgbFile);
        if (alphaFile)
            std::fclose(alphaFile);
        xgpu_plume_f2_log(
            "dxtdump failed guest=%08X rgb=%s alpha=%s",
            guest, rgbPath, alphaPath);
        return;
    }
    std::fprintf(rgbFile, "P6\n%u %u\n255\n", width, height);
    std::fwrite(rgb.data(), 1, rgb.size(), rgbFile);
    std::fclose(rgbFile);
    std::fprintf(alphaFile, "P5\n%u %u\n255\n", width, height);
    std::fwrite(alpha.data(), 1, alpha.size(), alphaFile);
    std::fclose(alphaFile);
    xgpu_plume_f2_log(
        "dxtdump guest=%08X version=%016llX format=%02X size=%ux%u "
        "rgb=%s alpha=%s",
        guest, static_cast<unsigned long long>(version), format,
        width, height, rgbPath, alphaPath);
}

static bool decode_bc3_volume_level(const uint8_t *src, size_t srcBytes,
                                    uint32_t width, uint32_t height,
                                    uint32_t depth, uint8_t *dst,
                                    uint32_t dstRowPitch,
                                    uint32_t dstSlicePitch)
{
    const uint32_t blocksX = (width + 3u) / 4u;
    const uint32_t blocksY = (height + 3u) / 4u;
    const size_t required = (size_t)blocksX * blocksY * depth * 16u;
    if (!src || !dst || required > srcBytes)
        return false;
    for (uint32_t z = 0; z < depth; z++) {
        for (uint32_t by = 0; by < blocksY; by++) {
            for (uint32_t bx = 0; bx < blocksX; bx++) {
                uint8_t rgba[16][4];
                /*
                 * NV2A volume S3TC is not host slice-major. Four adjacent Z
                 * slices are interleaved inside each XY block, matching
                 * xemu's s3tc_decompress_3d ordering.
                 */
                const size_t blockIndex = plumeBc3VolumeBlockIndex(
                    width, height, depth, bx, by, z);
                if (blockIndex >= required / 16u)
                    return false;
                decode_bc3_block(src + blockIndex * 16u, rgba);
                for (uint32_t py = 0; py < 4; py++) {
                    const uint32_t y = by * 4u + py;
                    if (y >= height)
                        continue;
                    for (uint32_t px = 0; px < 4; px++) {
                        const uint32_t x = bx * 4u + px;
                        if (x >= width)
                            continue;
                        std::memcpy(dst + (size_t)z * dstSlicePitch +
                                    (size_t)y * dstRowPitch + x * 4u,
                                    rgba[py * 4u + px], 4);
                    }
                }
            }
        }
    }
    return true;
}

void PlumeDraw::reset()
{
    *this = PlumeDraw{};
}

bool PlumeDraw::configureRenderExtent(uint32_t logicalWidth,
                                      uint32_t logicalHeight,
                                      uint32_t internalResolutionScale)
{
    uint32_t physicalWidth = 0;
    uint32_t physicalHeight = 0;
    if (m_pipelinesReady ||
        !plumeScaledExtent(logicalWidth, logicalHeight,
                           internalResolutionScale,
                           &physicalWidth, &physicalHeight))
        return false;
    m_outputWidth = logicalWidth;
    m_outputHeight = logicalHeight;
    m_internalResolutionScale = internalResolutionScale;
    return true;
}

bool PlumeDraw::reconfigureRenderExtent(
    PlumeContext &ctx, uint32_t logicalWidth, uint32_t logicalHeight,
    uint32_t internalResolutionScale)
{
    uint32_t physicalWidth = 0;
    uint32_t physicalHeight = 0;
    if (!m_pipelinesReady || hasQueuedWork() ||
        !plumeScaledExtent(logicalWidth, logicalHeight,
                           internalResolutionScale,
                           &physicalWidth, &physicalHeight)) {
        return false;
    }
    if (logicalWidth == m_outputWidth && logicalHeight == m_outputHeight &&
        internalResolutionScale == m_internalResolutionScale) {
        return true;
    }

    std::unordered_map<uint64_t, PlumeColorSurface> newColor;
    std::unordered_map<uint64_t, PlumeZetaSurface> newZeta;
    std::unordered_map<uint64_t, PlumeFramebuffer> newFramebuffers;
    std::vector<std::unique_ptr<RenderFramebuffer>> scaleFramebuffers;

    const RenderSampler *immutableTexSampler = m_texSampler.get();
    RenderDescriptorRange ranges[2] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1,
                              &immutableTexSampler),
    };
    RenderDescriptorSetDesc descriptorDesc(ranges, 2);

    for (const auto &entry : m_surfaceCache) {
        const PlumeColorSurface &old = entry.second;
        PlumeColorSurface surface;
        surface.width = old.width;
        surface.height = old.height;
        surface.guestAddr = old.guestAddr;
        surface.guestPitch = old.guestPitch;
        surface.guestFormat = old.guestFormat;
        surface.guestLayout = old.guestLayout;
        surface.contentSerial = old.contentSerial;
        surface.resolvedSerial = old.resolvedSerial;
        if (!plumeScaledExtent(surface.width, surface.height,
                               internalResolutionScale,
                               &surface.physicalWidth,
                               &surface.physicalHeight)) {
            return false;
        }
        surface.texture = ctx.device()->createTexture(
            RenderTextureDesc::ColorTarget(surface.physicalWidth,
                                           surface.physicalHeight,
                                           RenderFormat::B8G8R8A8_UNORM));
        surface.snapshot = ctx.device()->createTexture(
            RenderTextureDesc::ColorTarget(surface.physicalWidth,
                                           surface.physicalHeight,
                                           RenderFormat::B8G8R8A8_UNORM));
        if (!surface.texture || !surface.snapshot)
            return false;
        surface.view = surface.texture->createTextureView(
            RenderTextureViewDesc::Texture2D(RenderFormat::B8G8R8A8_UNORM));
        surface.snapshotView = surface.snapshot->createTextureView(
            RenderTextureViewDesc::Texture2D(RenderFormat::B8G8R8A8_UNORM));
        surface.descSet = ctx.device()->createDescriptorSet(descriptorDesc);
        surface.snapshotDescSet =
            ctx.device()->createDescriptorSet(descriptorDesc);
        if (!surface.view || !surface.snapshotView || !surface.descSet ||
            !surface.snapshotDescSet) {
            return false;
        }
        surface.descSet->setTexture(0, surface.texture.get(),
                                    RenderTextureLayout::SHADER_READ,
                                    surface.view.get());
        surface.snapshotDescSet->setTexture(
            0, surface.snapshot.get(), RenderTextureLayout::SHADER_READ,
            surface.snapshotView.get());
        newColor.emplace(entry.first, std::move(surface));
    }

    for (const auto &entry : m_zetaCache) {
        const PlumeZetaSurface &old = entry.second;
        PlumeZetaSurface zeta;
        zeta.format = old.format;
        zeta.width = old.width;
        zeta.height = old.height;
        zeta.guestAddr = old.guestAddr;
        zeta.guestPitch = old.guestPitch;
        zeta.contentSerial = old.contentSerial;
        zeta.downloadedSerial = 0;
        zeta.convertedSerial = 0;
        if (!plumeScaledExtent(zeta.width, zeta.height,
                               internalResolutionScale,
                               &zeta.physicalWidth,
                               &zeta.physicalHeight)) {
            return false;
        }
        zeta.texture = ctx.device()->createTexture(
            RenderTextureDesc::DepthTarget(zeta.physicalWidth,
                                           zeta.physicalHeight,
                                           zeta.format));
        if (!zeta.texture)
            return false;
        newZeta.emplace(entry.first, std::move(zeta));
    }

    for (const auto &entry : m_framebufferCache) {
        const PlumeFramebuffer &old = entry.second;
        auto color = newColor.find(old.colorGeneration);
        auto zeta = newZeta.end();
        if (color == newColor.end())
            return false;
        if (old.zetaGeneration) {
            zeta = newZeta.find(old.zetaGeneration);
            if (zeta == newZeta.end())
                return false;
        }
        const RenderTexture *colorTexture = color->second.texture.get();
        const RenderTexture *depthTexture = zeta != newZeta.end()
            ? zeta->second.texture.get() : nullptr;
        PlumeFramebuffer framebuffer;
        framebuffer.framebuffer = ctx.device()->createFramebuffer(
            RenderFramebufferDesc(&colorTexture, 1, depthTexture));
        if (!framebuffer.framebuffer)
            return false;
        framebuffer.colorGeneration = old.colorGeneration;
        framebuffer.zetaGeneration = old.zetaGeneration;
        newFramebuffers.emplace(entry.first, std::move(framebuffer));
    }

    bool recorded = false;
    bool transitionOk = true;
    RenderCommandList *command = ctx.uploadCmd();
    command->begin();
    for (auto &entry : newColor) {
        auto old = m_surfaceCache.find(entry.first);
        PlumeColorSurface &replacement = entry.second;
        if (old == m_surfaceCache.end() ||
            old->second.layout == RenderTextureLayout::UNKNOWN) {
            continue;
        }
        const RenderTexture *colorTexture = replacement.texture.get();
        std::unique_ptr<RenderFramebuffer> framebuffer =
            ctx.device()->createFramebuffer(
                RenderFramebufferDesc(&colorTexture, 1, nullptr));
        if (!framebuffer) {
            transitionOk = false;
            break;
        }
        const RenderTextureBarrier barriers[2] = {
            RenderTextureBarrier(old->second.texture.get(),
                                 RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(replacement.texture.get(),
                                 RenderTextureLayout::COLOR_WRITE),
        };
        command->barriers(RenderBarrierStage::GRAPHICS, nullptr, 0,
                          barriers, 2);
        if (!recordOutputScale(ctx, command, old->second.texture.get(),
                               old->second.view.get(), framebuffer.get(),
                               replacement.physicalWidth,
                               replacement.physicalHeight)) {
            transitionOk = false;
            break;
        }
        replacement.layout = RenderTextureLayout::COLOR_WRITE;
        old->second.layout = RenderTextureLayout::SHADER_READ;
        scaleFramebuffers.push_back(std::move(framebuffer));
        recorded = true;
    }
    command->end();
    if (recorded) {
        const RenderCommandList *submitted = command;
        ctx.queue()->executeCommandLists(&submitted, 1, nullptr, 0,
                                         nullptr, 0, ctx.fence());
        ctx.queue()->waitForCommandFence(ctx.fence());
    }
    releaseOutputScaleDescriptors();
    if (!transitionOk)
        return false;

    m_surfaceCache = std::move(newColor);
    m_zetaCache = std::move(newZeta);
    m_framebufferCache = std::move(newFramebuffers);
    m_outputWidth = logicalWidth;
    m_outputHeight = logicalHeight;
    m_internalResolutionScale = internalResolutionScale;
    m_replayWritten.clear();
    m_pendingDownloads.clear();
    m_deferredPresentDownloads.clear();
    return true;
}

uint32_t PlumeDraw::physicalOutputWidth() const
{
    uint32_t width = 0;
    uint32_t height = 0;
    return plumeScaledExtent(m_outputWidth, m_outputHeight,
                             m_internalResolutionScale, &width, &height)
        ? width : 0;
}

uint32_t PlumeDraw::physicalOutputHeight() const
{
    uint32_t width = 0;
    uint32_t height = 0;
    return plumeScaledExtent(m_outputWidth, m_outputHeight,
                             m_internalResolutionScale, &width, &height)
        ? height : 0;
}

bool PlumeDraw::initPipelines(PlumeContext &ctx)
{
    if (m_pipelinesReady)
        return true;
    if (ctx.failed() || !ctx.ready())
        return false;
    m_recordShaderTarget = shaderTargetForRenderFormat(
        ctx.iface()->getCapabilities().shaderFormat);

    /* Fullscreen scene filter. Render targets stay at their integer-scaled
     * physical extent; this is the only place the final scene is fitted to
     * the independently-sized host swapchain. */
    {
        static const char *kOutputScaleHlsl =
            "Texture2D<float4> sourceTexture : register(t0);\n"
            "SamplerState sourceSampler : register(s1);\n"
            "struct VSOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };\n"
            "VSOut VSMain(uint id : SV_VertexID) {\n"
            "  float2 uv = float2((id << 1) & 2, id & 2);\n"
            "  VSOut o; o.uv = uv;\n"
            "  o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
            "  return o;\n"
            "}\n"
            "float4 PSMain(VSOut i) : SV_Target {\n"
            "  return sourceTexture.Sample(sourceSampler, i.uv);\n"
            "}\n";
        ShaderCompileResult vs = compileForContext(
            ctx, kOutputScaleHlsl, "VSMain", "vs_6_0");
        ShaderCompileResult ps = compileForContext(
            ctx, kOutputScaleHlsl, "PSMain", "ps_6_0");
        if (vs.ok && ps.ok && vs.target == ps.target) {
            const RenderShaderFormat format =
                renderShaderFormatForTarget(vs.target);
            m_outputScaleVS = ctx.device()->createShader(
                vs.bytecode.data(), vs.bytecode.size(),
                vs.entryPoint.c_str(), format);
            m_outputScalePS = ctx.device()->createShader(
                ps.bytecode.data(), ps.bytecode.size(),
                ps.entryPoint.c_str(), format);

            RenderSamplerDesc samplerDesc;
            samplerDesc.minFilter = RenderFilter::LINEAR;
            samplerDesc.magFilter = RenderFilter::LINEAR;
            samplerDesc.mipmapMode = RenderMipmapMode::NEAREST;
            samplerDesc.addressU = RenderTextureAddressMode::CLAMP;
            samplerDesc.addressV = RenderTextureAddressMode::CLAMP;
            samplerDesc.addressW = RenderTextureAddressMode::CLAMP;
            m_outputScaleSampler =
                ctx.device()->createSampler(samplerDesc);

            const RenderSampler *immutableSampler =
                m_outputScaleSampler.get();
            RenderDescriptorRange ranges[2] = {
                RenderDescriptorRange(
                    RenderDescriptorRangeType::TEXTURE, 0, 1),
                RenderDescriptorRange(
                    RenderDescriptorRangeType::SAMPLER, 1, 1,
                    &immutableSampler),
            };
            RenderDescriptorSetDesc descriptorDesc(ranges, 2);
            RenderPipelineLayoutDesc layoutDesc;
            layoutDesc.descriptorSetDescs = &descriptorDesc;
            layoutDesc.descriptorSetDescsCount = 1;
            m_outputScaleLayout =
                ctx.device()->createPipelineLayout(layoutDesc);

            RenderGraphicsPipelineDesc pipelineDesc;
            pipelineDesc.pipelineLayout = m_outputScaleLayout.get();
            pipelineDesc.vertexShader = m_outputScaleVS.get();
            pipelineDesc.pixelShader = m_outputScalePS.get();
            pipelineDesc.renderTargetFormat[0] =
                RenderFormat::B8G8R8A8_UNORM;
            pipelineDesc.renderTargetBlend[0] = RenderBlendDesc::Copy();
            pipelineDesc.renderTargetCount = 1;
            pipelineDesc.primitiveTopology =
                RenderPrimitiveTopology::TRIANGLE_LIST;
            m_outputScalePso =
                ctx.device()->createGraphicsPipeline(pipelineDesc);
            pipelineDesc.renderTargetBlend[0] =
                RenderBlendDesc::AlphaBlend();
            m_outputOverlayPso =
                ctx.device()->createGraphicsPipeline(pipelineDesc);
            m_outputScaleReady = m_outputScaleVS && m_outputScalePS &&
                m_outputScaleSampler && m_outputScaleLayout &&
                m_outputScalePso && m_outputOverlayPso;
        }
        if (!m_outputScaleReady) {
            std::fprintf(stderr,
                         "[PLUME] output scale pipeline unavailable\n");
            return false;
        }
    }

    /* Geometry pipeline: XYZRHW screen-space -> NDC using active-target half size. */
    {
        const std::string kGeom = std::string(ndcScaleHlslDeclaration()) +
            "struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "float4 xform(float4 p) {\n"
            "    float w = ndcScale.homogeneous != 0.0 && abs(p.w) > 1.0e-20 ? rcp(p.w) : 1.0;\n"
            "    float4 o; o.x = (p.x + ndcScale.screenSpaceOffset) / ndcScale.halfW - 1.0;\n"
            "    o.y = 1.0 - (p.y + ndcScale.screenSpaceOffset) / ndcScale.halfH;\n"
            "    o.z = p.z; o.xyz *= w; o.w = w; return o;\n"
            "}\n"
            "struct VSInD { float4 pos : POSITION; float4 col : COLOR; };\n"
            "VSOut VSDiffuse(VSInD i) { VSOut o; o.pos = xform(i.pos); o.col = i.col.bgra; return o; }\n"
            "struct VSInP { float4 pos : POSITION; };\n"
            "VSOut VSPlain(VSInP i) { VSOut o; o.pos = xform(i.pos); o.col = float4(0.7, 0.7, 0.7, 0.55); return o; }\n"
            "float4 PSMain(VSOut i) : SV_TARGET { return i.col; }\n";
        ShaderCompileResult gvd = compileForContext(ctx, kGeom.c_str(), "VSDiffuse", "vs_6_0");
        ShaderCompileResult gvp = compileForContext(ctx, kGeom.c_str(), "VSPlain", "vs_6_0");
        ShaderCompileResult gps = compileForContext(ctx, kGeom.c_str(), "PSMain", "ps_6_0");
        if (gvd.ok && gvp.ok && gps.ok) {
            const RenderShaderFormat fmt = renderShaderFormatForTarget(gvd.target);
            m_geomVS = ctx.device()->createShader(gvd.bytecode.data(), gvd.bytecode.size(), gvd.entryPoint.c_str(), fmt);
            m_geomVSPlain = ctx.device()->createShader(gvp.bytecode.data(), gvp.bytecode.size(), gvp.entryPoint.c_str(), fmt);
            m_geomPS = ctx.device()->createShader(gps.bytecode.data(), gps.bytecode.size(), gps.entryPoint.c_str(), fmt);
            RenderPushConstantRange ndcPC(
                0, 0, 0, sizeof(ProgramVertexPushConstants),
                RenderShaderStageFlag::VERTEX);
            RenderPipelineLayoutDesc ld;
            ld.pushConstantRanges = &ndcPC;
            ld.pushConstantRangesCount = 1;
            ld.allowInputLayout = true;
            m_geomLayout = ctx.device()->createPipelineLayout(ld);
            m_geomReady = true;
        } else {
            fprintf(stderr, "[PLUME] present: geometry shader compile failed (draws disabled)\n");
        }
    }

    /* Textured pipeline: POSITION + optional diffuse + TEXCOORD. */
    if (m_geomReady) {
        const std::string kTex = std::string(ndcScaleHlslDeclaration()) +
            "Texture2D<float4> tex : register(t0);\n"
            "SamplerState smp : register(s1);\n"
            "struct VSInT { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
            "struct VSInTD { float4 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
            "struct VSOutT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
            "VSOutT VSTexFill(float4 pos, float4 col, float2 uv) {\n"
            "    float w = ndcScale.homogeneous != 0.0 && abs(pos.w) > 1.0e-20 ? rcp(pos.w) : 1.0;\n"
            "    VSOutT o;\n"
            "    o.pos.x = (pos.x + ndcScale.screenSpaceOffset) / ndcScale.halfW - 1.0;\n"
            "    o.pos.y = 1.0 - (pos.y + ndcScale.screenSpaceOffset) / ndcScale.halfH;\n"
            "    o.pos.z = pos.z; o.pos.xyz *= w; o.pos.w = w;\n"
            "    o.col = col; o.uv = uv; return o;\n"
            "}\n"
            "VSOutT VSTexMain(VSInT i) {\n"
            "    return VSTexFill(i.pos, float4(1, 1, 1, 1), i.uv);\n"
            "}\n"
            "VSOutT VSTexDiffuse(VSInTD i) { return VSTexFill(i.pos, i.col.bgra, i.uv); }\n"
            "float4 PSTexMain(VSOutT i) : SV_TARGET { return tex.Sample(smp, i.uv) * i.col; }\n";
        ShaderCompileResult tvs = compileForContext(ctx, kTex.c_str(), "VSTexMain", "vs_6_0");
        ShaderCompileResult tvsd = compileForContext(ctx, kTex.c_str(), "VSTexDiffuse", "vs_6_0");
        ShaderCompileResult tps = compileForContext(ctx, kTex.c_str(), "PSTexMain", "ps_6_0");
        if (tvs.ok && tvsd.ok && tps.ok) {
            const RenderShaderFormat fmt = renderShaderFormatForTarget(tvs.target);
            m_texVS = ctx.device()->createShader(tvs.bytecode.data(), tvs.bytecode.size(), tvs.entryPoint.c_str(), fmt);
            m_texVSDiffuse = ctx.device()->createShader(tvsd.bytecode.data(), tvsd.bytecode.size(), tvsd.entryPoint.c_str(), fmt);
            m_texPS = ctx.device()->createShader(tps.bytecode.data(), tps.bytecode.size(), tps.entryPoint.c_str(), fmt);

            RenderSamplerDesc sd;
            sd.minFilter = RenderFilter::LINEAR; sd.magFilter = RenderFilter::LINEAR;
            sd.addressU = sd.addressV = sd.addressW = RenderTextureAddressMode::WRAP;
            m_texSampler = ctx.device()->createSampler(sd);

            const RenderSampler *immutableTexSampler = m_texSampler.get();
            RenderDescriptorRange ranges[2] = {
                RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
                RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1,
                                      &immutableTexSampler),
            };
            RenderDescriptorSetDesc dsd(ranges, 2);
            RenderPushConstantRange ndcPC(
                0, 0, 0, sizeof(ProgramVertexPushConstants),
                RenderShaderStageFlag::VERTEX);
            RenderPipelineLayoutDesc tld;
            tld.pushConstantRanges = &ndcPC;
            tld.pushConstantRangesCount = 1;
            tld.descriptorSetDescs = &dsd;
            tld.descriptorSetDescsCount = 1;
            tld.allowInputLayout = true;
            m_texLayout = ctx.device()->createPipelineLayout(tld);
            m_texReady = true;
        } else {
            fprintf(stderr, "[PLUME] present: textured shader compile failed (textures disabled)\n");
        }
    }

    /* Programmable pixel-shader pipeline: translated xps.1.1 -> HLSL PS, bound
     * with tex0-3 (t0-3) + samp0-3 (s4-7) + cbuffer c[8] (b8). Draws that have
     * an active pixel shader route here; the VS supplies pos + diffuse + uv. */
    if (m_texReady) {
        /* 1x1 white fallback for unbound texture stages */
        m_whiteTex = ctx.device()->createTexture(
            RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::B8G8R8A8_UNORM));
        m_whiteCubeTex = ctx.device()->createTexture(RenderTextureDesc::Texture(
            RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6,
            RenderFormat::B8G8R8A8_UNORM, RenderTextureFlag::CUBE));
        if (m_whiteTex) {
            m_whiteView = m_whiteTex->createTextureView(
                RenderTextureViewDesc::Texture2D(RenderFormat::B8G8R8A8_UNORM));
            if (m_whiteCubeTex) {
                m_whiteCubeView = m_whiteCubeTex->createTextureView(
                    RenderTextureViewDesc::TextureCube(
                        RenderFormat::B8G8R8A8_UNORM));
            }
            uint8_t px[256] = { 255, 255, 255, 255 };
            std::unique_ptr<RenderBuffer> st =
                ctx.device()->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(px)));
            if (m_whiteView && m_whiteCubeView && st &&
                copyToMappedBuffer(st.get(), px, sizeof(px))) {
                RenderCommandList *up = ctx.uploadCmd();
                up->begin();
                up->barriers(
                    RenderBarrierStage::COPY,
                    RenderTextureBarrier(
                        m_whiteTex.get(), RenderTextureLayout::COPY_DEST));
                up->barriers(
                    RenderBarrierStage::COPY,
                    RenderTextureBarrier(
                        m_whiteCubeTex.get(), RenderTextureLayout::COPY_DEST));
                RenderTextureCopyLocation csrc =
                    RenderTextureCopyLocation::PlacedFootprint(
                        st.get(), RenderFormat::B8G8R8A8_UNORM,
                        1, 1, 1, 64, 0);
                RenderTextureCopyLocation cdst =
                    RenderTextureCopyLocation::Subresource(
                        m_whiteTex.get(), 0, 0);
                up->copyTextureRegion(cdst, csrc, 0, 0, 0, nullptr);
                for (uint32_t face = 0; face < 6; ++face) {
                    RenderTextureCopyLocation cubeDst =
                        RenderTextureCopyLocation::Subresource(
                            m_whiteCubeTex.get(), 0, face);
                    up->copyTextureRegion(cubeDst, csrc, 0, 0, 0, nullptr);
                }
                up->barriers(
                    RenderBarrierStage::GRAPHICS,
                    RenderTextureBarrier(
                        m_whiteTex.get(), RenderTextureLayout::SHADER_READ));
                up->barriers(
                    RenderBarrierStage::GRAPHICS,
                    RenderTextureBarrier(
                        m_whiteCubeTex.get(), RenderTextureLayout::SHADER_READ));
                up->end();
                const RenderCommandList *cl = up;
                ctx.queue()->executeCommandLists(
                    &cl, 1, nullptr, 0, nullptr, 0, ctx.fence());
                uint64_t wait_t0 = xgpu_plume_wait_stats_begin();
                ctx.queue()->waitForCommandFence(ctx.fence());
                xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_TEX_UPLOAD, wait_t0);
            } else {
                std::fprintf(stderr,
                             "[PLUME] white fallback upload unavailable\n");
                m_whiteView.reset();
                m_whiteTex.reset();
                m_whiteCubeView.reset();
                m_whiteCubeTex.reset();
            }
        }

        RenderSamplerDesc psd;
        psd.minFilter = RenderFilter::LINEAR; psd.magFilter = RenderFilter::LINEAR;
        psd.addressU = psd.addressV = psd.addressW = RenderTextureAddressMode::WRAP;
        m_progSampler = ctx.device()->createSampler(psd);

        const std::string kProgVS = std::string(ndcScaleHlslDeclaration()) +
            "struct VSOut { float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
            "  float4 uv0:TEXCOORD0; float4 uv1:TEXCOORD1; float4 uv2:TEXCOORD2; float4 uv3:TEXCOORD3; };\n"
            "float4 xf(float4 p){ float w=ndcScale.homogeneous!=0.0&&abs(p.w)>1.0e-20?rcp(p.w):1.0; float4 o;\n"
            "  o.x=(p.x+ndcScale.screenSpaceOffset)/ndcScale.halfW-1.0;\n"
            "  o.y=1.0-(p.y+ndcScale.screenSpaceOffset)/ndcScale.halfH; o.z=p.z; o.xyz*=w; o.w=w; return o; }\n"
            "VSOut fill1(float4 pos, float4 col0, float4 col1, float4 uv){\n"
            "  VSOut o; o.pos=xf(pos); o.col0=col0; o.col1=col1;\n"
            "  o.uv0=uv; o.uv1=uv; o.uv2=uv; o.uv3=uv; return o; }\n"
            "VSOut fill2(float4 pos, float4 col0, float4 col1, float4 uv0, float4 uv1){\n"
            "  VSOut o; o.pos=xf(pos); o.col0=col0; o.col1=col1;\n"
            "  o.uv0=uv0; o.uv1=uv1; o.uv2=uv0; o.uv3=uv0; return o; }\n"
            "VSOut fill3(float4 pos, float4 col0, float4 col1, float4 uv0, float4 uv1, float4 uv2){\n"
            "  VSOut o; o.pos=xf(pos); o.col0=col0; o.col1=col1;\n"
            "  o.uv0=uv0; o.uv1=uv1; o.uv2=uv2; o.uv3=uv0; return o; }\n"
            "VSOut fill4(float4 pos, float4 col0, float4 col1,\n"
            "            float4 uv0, float4 uv1, float4 uv2, float4 uv3){\n"
            "  VSOut o; o.pos=xf(pos); o.col0=col0; o.col1=col1;\n"
            "  o.uv0=uv0; o.uv1=uv1; o.uv2=uv2; o.uv3=uv3; return o; }\n"
            "struct In000{ float4 pos:POSITION; };\n"
            "VSOut VS000(In000 i){ return fill1(i.pos,float4(1,1,1,1),float4(0,0,0,1),float4(0,0,0,1)); }\n"
            "struct In001{ float4 pos:POSITION; float4 uv:TEXCOORD0; };\n"
            "VSOut VS001(In001 i){ return fill1(i.pos,float4(1,1,1,1),float4(0,0,0,1),i.uv); }\n"
            "struct In0012{ float4 pos:POSITION; float4 uv0:TEXCOORD0; float4 uv1:TEXCOORD1; };\n"
            "VSOut VS0012(In0012 i){ return fill2(i.pos,float4(1,1,1,1),float4(0,0,0,1),i.uv0,i.uv1); }\n"
            "struct In0013{ float4 pos:POSITION; float4 uv0:TEXCOORD0; float4 uv1:TEXCOORD1; float4 uv2:TEXCOORD2; };\n"
            "VSOut VS0013(In0013 i){ return fill3(i.pos,float4(1,1,1,1),float4(0,0,0,1),i.uv0,i.uv1,i.uv2); }\n"
            "struct In0014{ float4 pos:POSITION; float4 uv0:TEXCOORD0; float4 uv1:TEXCOORD1;\n"
            "  float4 uv2:TEXCOORD2; float4 uv3:TEXCOORD3; };\n"
            "VSOut VS0014(In0014 i){ return fill4(i.pos,float4(1,1,1,1),float4(0,0,0,1),i.uv0,i.uv1,i.uv2,i.uv3); }\n"
            "struct In010{ float4 pos:POSITION; float4 spec:COLOR1; };\n"
            "VSOut VS010(In010 i){ return fill1(i.pos,float4(1,1,1,1),i.spec.bgra,float4(0,0,0,1)); }\n"
            "struct In011{ float4 pos:POSITION; float4 spec:COLOR1; float4 uv:TEXCOORD0; };\n"
            "VSOut VS011(In011 i){ return fill1(i.pos,float4(1,1,1,1),i.spec.bgra,i.uv); }\n"
            "struct In0112{ float4 pos:POSITION; float4 spec:COLOR1; float4 uv0:TEXCOORD0; float4 uv1:TEXCOORD1; };\n"
            "VSOut VS0112(In0112 i){ return fill2(i.pos,float4(1,1,1,1),i.spec.bgra,i.uv0,i.uv1); }\n"
            "struct In0113{ float4 pos:POSITION; float4 spec:COLOR1; float4 uv0:TEXCOORD0; float4 uv1:TEXCOORD1; float4 uv2:TEXCOORD2; };\n"
            "VSOut VS0113(In0113 i){ return fill3(i.pos,float4(1,1,1,1),i.spec.bgra,i.uv0,i.uv1,i.uv2); }\n"
            "struct In0114{ float4 pos:POSITION; float4 spec:COLOR1; float4 uv0:TEXCOORD0;\n"
            "  float4 uv1:TEXCOORD1; float4 uv2:TEXCOORD2; float4 uv3:TEXCOORD3; };\n"
            "VSOut VS0114(In0114 i){ return fill4(i.pos,float4(1,1,1,1),i.spec.bgra,i.uv0,i.uv1,i.uv2,i.uv3); }\n"
            "struct In100{ float4 pos:POSITION; float4 col:COLOR0; };\n"
            "VSOut VS100(In100 i){ return fill1(i.pos,i.col.bgra,float4(0,0,0,1),float4(0,0,0,1)); }\n"
            "struct In101{ float4 pos:POSITION; float4 col:COLOR0; float4 uv:TEXCOORD0; };\n"
            "VSOut VS101(In101 i){ return fill1(i.pos,i.col.bgra,float4(0,0,0,1),i.uv); }\n"
            "struct In1012{ float4 pos:POSITION; float4 col:COLOR0; float4 uv0:TEXCOORD0; float4 uv1:TEXCOORD1; };\n"
            "VSOut VS1012(In1012 i){ return fill2(i.pos,i.col.bgra,float4(0,0,0,1),i.uv0,i.uv1); }\n"
            "struct In1013{ float4 pos:POSITION; float4 col:COLOR0; float4 uv0:TEXCOORD0; float4 uv1:TEXCOORD1; float4 uv2:TEXCOORD2; };\n"
            "VSOut VS1013(In1013 i){ return fill3(i.pos,i.col.bgra,float4(0,0,0,1),i.uv0,i.uv1,i.uv2); }\n"
            "struct In1014{ float4 pos:POSITION; float4 col:COLOR0; float4 uv0:TEXCOORD0;\n"
            "  float4 uv1:TEXCOORD1; float4 uv2:TEXCOORD2; float4 uv3:TEXCOORD3; };\n"
            "VSOut VS1014(In1014 i){ return fill4(i.pos,i.col.bgra,float4(0,0,0,1),i.uv0,i.uv1,i.uv2,i.uv3); }\n"
            "struct In110{ float4 pos:POSITION; float4 col:COLOR0; float4 spec:COLOR1; };\n"
            "VSOut VS110(In110 i){ return fill1(i.pos,i.col.bgra,i.spec.bgra,float4(0,0,0,1)); }\n"
            "struct In111{ float4 pos:POSITION; float4 col:COLOR0; float4 spec:COLOR1; float4 uv:TEXCOORD0; };\n"
            "VSOut VS111(In111 i){ return fill1(i.pos,i.col.bgra,i.spec.bgra,i.uv); }\n"
            "struct In1112{ float4 pos:POSITION; float4 col:COLOR0; float4 spec:COLOR1; float4 uv0:TEXCOORD0; float4 uv1:TEXCOORD1; };\n"
            "VSOut VS1112(In1112 i){ return fill2(i.pos,i.col.bgra,i.spec.bgra,i.uv0,i.uv1); }\n"
            "struct In1113{ float4 pos:POSITION; float4 col:COLOR0; float4 spec:COLOR1; float4 uv0:TEXCOORD0; float4 uv1:TEXCOORD1; float4 uv2:TEXCOORD2; };\n"
            "VSOut VS1113(In1113 i){ return fill3(i.pos,i.col.bgra,i.spec.bgra,i.uv0,i.uv1,i.uv2); }\n"
            "struct In1114{ float4 pos:POSITION; float4 col:COLOR0; float4 spec:COLOR1;\n"
            "  float4 uv0:TEXCOORD0; float4 uv1:TEXCOORD1; float4 uv2:TEXCOORD2; float4 uv3:TEXCOORD3; };\n"
            "VSOut VS1114(In1114 i){ return fill4(i.pos,i.col.bgra,i.spec.bgra,i.uv0,i.uv1,i.uv2,i.uv3); }\n";
        const char *entA[20] = {
            "VS000", "VS001", "VS0012", "VS0013", "VS0014",
            "VS010", "VS011", "VS0112", "VS0113", "VS0114",
            "VS100", "VS101", "VS1012", "VS1013", "VS1014",
            "VS110", "VS111", "VS1112", "VS1113", "VS1114"
        };
        bool vsok = true;
        for (int k = 0; k < 20; k++) {
            ShaderCompileResult shader =
                compileForContext(ctx, kProgVS.c_str(), entA[k], "vs_6_0");
            if (!shader.ok) {
                vsok = false;
                break;
            }
            const RenderShaderFormat format =
                renderShaderFormatForTarget(shader.target);
            m_progVS[k] = ctx.device()->createShader(
                shader.bytecode.data(), shader.bytecode.size(),
                shader.entryPoint.c_str(), format);
        }

        const ProgramBindingLayout bindingLayout = makeProgramBindingLayout();
        RenderDescriptorSetDesc pdsd(bindingLayout.ranges.data(),
                                     bindingLayout.rangeCount);
        /* Push binding 0: screen/depth transform. b8: pixel constants. */
        RenderPushConstantRange ndcPC(
            0, 0, 0, sizeof(ProgramVertexPushConstants),
            RenderShaderStageFlag::VERTEX);
        RenderPipelineLayoutDesc pld;
        pld.pushConstantRanges = &ndcPC;
        pld.pushConstantRangesCount = 1;
        pld.descriptorSetDescs = &pdsd;
        pld.descriptorSetDescsCount = 1;
        pld.allowInputLayout = true;
        m_progLayout = ctx.device()->createPipelineLayout(pld);

        /* Indexed programmable draws add the portable t4 structured VS
         * constant stream to the regular t0-3/s4-7/b8 descriptor set. NV2A
         * oPos is screen-space, so the translated VS uses the same target-size
         * push constant to reconstruct host clip-space position. */
        RenderPushConstantRange piNdcPC(
            0, 0, 0, sizeof(ProgramVertexPushConstants),
            RenderShaderStageFlag::VERTEX);
        const ProgramBindingLayout indexedBindingLayout =
            makeProgramIndexedBindingLayout();
        RenderDescriptorSetDesc piDsd(indexedBindingLayout.ranges.data(),
                                      indexedBindingLayout.rangeCount);
        RenderPipelineLayoutDesc piPld;
        piPld.pushConstantRanges = &piNdcPC;
        piPld.pushConstantRangesCount = 1;
        piPld.descriptorSetDescs = &piDsd;
        piPld.descriptorSetDescsCount = 1;
        piPld.allowInputLayout = true;
        m_progIdxLayout = ctx.device()->createPipelineLayout(piPld);

        m_progReady = vsok && m_whiteTex && m_whiteView &&
                      m_whiteCubeTex && m_whiteCubeView && m_progSampler &&
                      m_progLayout && m_progIdxLayout;
        if (m_progReady) {
            PlumePixelShader fallback;
            fallback.hlsl = plumeFixedFallbackPixelShaderHlsl();
            fallback.ok = true;
            fallback.compileFuture = queueShaderCompile(
                m_recordShaderTarget, fallback.hlsl, "ps_6_0");
            m_fixedFallbackPS = m_psNext++;
            m_psReg.emplace(m_fixedFallbackPS, std::move(fallback));

            PlumePixelShader fallbackW;
            fallbackW.hlsl = plumeFixedFallbackWPixelShaderHlsl();
            fallbackW.ok = !fallbackW.hlsl.empty();
            if (fallbackW.ok) {
                fallbackW.compileFuture = queueShaderCompile(
                    m_recordShaderTarget, fallbackW.hlsl, "ps_6_0");
            }
            m_fixedFallbackPSW = m_psNext++;
            m_psReg.emplace(m_fixedFallbackPSW, std::move(fallbackW));
        }
        fprintf(stderr, "[PLUME] present: programmable PS path %s\n",
                m_progReady ? "ready" : "unavailable");
    }

    m_pipelinesReady = true;
    fprintf(stderr,
            "[PLUME] present: draw pipelines ready "
            "(logical=%ux%u physical=%ux%u scale=%ux)\n",
            m_outputWidth, m_outputHeight,
            physicalOutputWidth(), physicalOutputHeight(),
            m_internalResolutionScale);
    fflush(stderr);
    return true;
}

static RenderComparisonFunction plume_draw_compare_from_d3d(uint32_t func)
{
    if (func >= 1 && func <= 8)
        return static_cast<RenderComparisonFunction>(func);
    return RenderComparisonFunction::LESS_EQUAL;
}

static RenderCullMode plume_draw_cull_from_d3d(uint32_t cull)
{
    switch (cull) {
    case 2: return RenderCullMode::FRONT;
    case 3: return RenderCullMode::BACK;
    default: return RenderCullMode::NONE;
    }
}

static void plume_draw_apply_state(RenderGraphicsPipelineDesc &desc,
                                   const XgpuPlumeRenderState &state,
                                   bool hasDepthAttachment)
{
    float depthBias;
    float slopeScaledDepthBias;
    static_assert(sizeof(depthBias) == sizeof(state.depth_bias_bits));
    memcpy(&depthBias, &state.depth_bias_bits, sizeof(depthBias));
    memcpy(&slopeScaledDepthBias, &state.slope_scaled_depth_bias_bits,
           sizeof(slopeScaledDepthBias));
    desc.depthEnabled = hasDepthAttachment && state.depth_enable != 0;
    desc.depthWriteEnabled = hasDepthAttachment && state.depth_write != 0;
    desc.depthFunction = plume_draw_compare_from_d3d(state.depth_func);
    RenderFormat depthFormat = plume_depth_format_from_xgpu(
        state.zeta_format, state.zeta_float);
    desc.depthTargetFormat = hasDepthAttachment
        ? (depthFormat != RenderFormat::UNKNOWN
               ? depthFormat : RenderFormat::D32_FLOAT)
        : RenderFormat::UNKNOWN;
    desc.cullMode = plume_draw_cull_from_d3d(state.cull_mode);
    desc.frontFace = RenderFrontFace::CLOCKWISE;
    /* NV2A clips at the near plane. The RHI default leaves depth clip
     * DISABLED, so triangles crossing w=0 (near-plane geometry restored
     * to clip space by xf()) rasterized as wrap-around garbage — the
     * "exploding polys" (bug-352). */
    desc.depthClipEnabled = true;
    desc.depthBias = static_cast<int32_t>(depthBias);
    desc.depthBiasClamp = 0.0f;
    desc.slopeScaledDepthBias = slopeScaledDepthBias;

    desc.renderTargetBlend[0] = plume_blend_desc_from_d3d(state);
    if (hasDepthAttachment)
        plume_apply_stencil_state(desc, state);
    else
        desc.stencilEnabled = false;
}

static XgpuPlumeRenderState plume_draw_default_state()
{
    XgpuPlumeRenderState state = {};
    state.depth_func = 8;
    state.blend_enable = 1;
    state.src_blend = 5;
    state.dst_blend = 6;
    state.blend_op = 1;
    state.cull_mode = 1;
    state.color_write_mask = 0xF;
    state.fixed_color_op[0] = 4;
    state.fixed_color_arg0[0] = 1;
    state.fixed_color_arg1[0] = 2;
    state.fixed_color_arg2[0] = 1;
    state.fixed_alpha_op[0] = 2;
    state.fixed_alpha_arg0[0] = 1;
    state.fixed_alpha_arg1[0] = 2;
    state.fixed_alpha_arg2[0] = 1;
    for (uint32_t stage = 1; stage < 4; ++stage) {
        state.fixed_color_op[stage] = 1;
        state.fixed_alpha_op[stage] = 1;
        state.fixed_color_arg0[stage] = 1;
        state.fixed_color_arg1[stage] = 2;
        state.fixed_color_arg2[stage] = 1;
        state.fixed_alpha_arg0[stage] = 1;
        state.fixed_alpha_arg1[stage] = 2;
        state.fixed_alpha_arg2[stage] = 1;
    }
    state.fixed_texture_factor = 0xFFFFFFFFu;
    return state;
}

::plume::RenderPipeline *PlumeDraw::geomPso(PlumeContext &ctx, uint32_t stride,
                                            uint8_t topology, uint8_t hasDiffuse,
                                            const XgpuPlumeRenderState &renderState,
                                            bool hasDepthAttachment)
{
    uint32_t baseKey = (stride << 9) | (uint32_t(topology) << 1) |
                       (hasDiffuse ? 1u : 0u);
    uint64_t key = plume_render_state_key(renderState) ^
                   (uint64_t(baseKey) * 0x9E3779B185EBCA87ull);
    key = (key * 1099511628211ull) ^ (hasDepthAttachment ? 1ull : 0ull);
    auto it = m_geomPsos.find(key);
    if (it != m_geomPsos.end())
        return it->second.get();

    RenderInputSlot slot(0, stride);
    RenderInputElement elems[2] = {
        RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32B32A32_FLOAT, 0, 0),
        RenderInputElement("COLOR", 0, 1, RenderFormat::R8G8B8A8_UNORM, 0, 16),  /* diffuse @16 */
    };
    RenderGraphicsPipelineDesc pd;
    pd.pipelineLayout = m_geomLayout.get();
    pd.vertexShader = hasDiffuse ? m_geomVS.get() : m_geomVSPlain.get();
    pd.pixelShader = m_geomPS.get();
    pd.inputSlots = &slot;
    pd.inputSlotsCount = 1;
    pd.inputElements = elems;
    pd.inputElementsCount = hasDiffuse ? 2u : 1u;
    pd.renderTargetFormat[0] = RenderFormat::B8G8R8A8_UNORM;
    pd.renderTargetCount = 1;
    pd.primitiveTopology = (RenderPrimitiveTopology)topology;
    plume_draw_apply_state(pd, renderState, hasDepthAttachment);

    XRECOMP_TRACY_ZONE_SCOPED("Plume Create Graphics Pipeline");
    uint64_t perf_pipeline_t0 = xgpu_plume_perf_begin();
    std::unique_ptr<RenderPipeline> pso =
        ctx.device()->createGraphicsPipeline(pd);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_PIPELINE, perf_pipeline_t0);
    RenderPipeline *raw = pso.get();
    m_geomPsos.emplace(key, std::move(pso));
    return raw;
}

/* Map an Xbox D3DFORMAT to a Plume format we can upload directly. */
static bool plume_map_texfmt(uint32_t d3dfmt, RenderFormat &out)
{
    switch (d3dfmt) {
    case kHostFrameFormat: out = RenderFormat::R8G8B8A8_UNORM; return true;
    case 0x0C: out = RenderFormat::BC1_UNORM;      return true;  /* DXT1 (=12) */
    case 0x0E: out = RenderFormat::BC2_UNORM;      return true;  /* DXT2/3 (=14) */
    case 0x0F: out = RenderFormat::BC3_UNORM;      return true;  /* DXT4/5 (=15) */
    case 0x06: out = RenderFormat::B8G8R8A8_UNORM; return true;  /* A8R8G8B8 */
    case 0x07: out = RenderFormat::B8G8R8A8_UNORM; return true;  /* X8R8G8B8 */
    case 0x11: out = RenderFormat::B8G8R8A8_UNORM; return true;  /* LIN_R5G6B5, converted */
    case 0x12: out = RenderFormat::B8G8R8A8_UNORM; return true;  /* LIN_A8R8G8B8 */
    case 0x1E: out = RenderFormat::B8G8R8A8_UNORM; return true;  /* LIN_X8R8G8B8 */
    case 0x19: out = RenderFormat::R8_UNORM;        return true;  /* A8 */
    case 0x1A: out = RenderFormat::R8G8_UNORM;      return true;  /* A8L8 */
    case 0x1F: out = RenderFormat::R8_UNORM;        return true;  /* LIN_A8 */
    case 0x20: out = RenderFormat::R8G8_UNORM;      return true;  /* LIN_A8L8 */
    case 0x24: out = RenderFormat::B8G8R8A8_UNORM;  return true;  /* YUY2, converted */
    case 0x25: out = RenderFormat::B8G8R8A8_UNORM;  return true;  /* UYVY, converted */
    case 0x35: out = RenderFormat::R16_UNORM;       return true;  /* LU_IMAGE_Y16 */
    default:   return false;
    }
}

static RenderTextureAddressMode plume_texture_address(uint32_t mode)
{
    switch (mode & 7u) {
    case 1: return RenderTextureAddressMode::WRAP;
    case 2: return RenderTextureAddressMode::MIRROR;
    case 3: return RenderTextureAddressMode::CLAMP;
    case 4: return RenderTextureAddressMode::BORDER;
    case 5: return RenderTextureAddressMode::CLAMP;
    default: return RenderTextureAddressMode::WRAP;
    }
}

static RenderBorderColor plume_border_color(uint32_t color)
{
    /*
     * Plume's portable sampler contract exposes the three border colors
     * shared by D3D12, Vulkan, and Metal. Preserve the guest's alpha-zero
     * contract first (the default Xbox border is transparent black), then
     * the two canonical opaque colors.
     */
    if ((color & 0xFF000000u) == 0)
        return RenderBorderColor::TRANSPARENT_BLACK;
    if (color == 0xFFFFFFFFu)
        return RenderBorderColor::OPAQUE_WHITE;
    return RenderBorderColor::OPAQUE_BLACK;
}

static uint64_t plume_sampler_key(const XgpuSamplerBinding &binding)
{
    uint64_t key = 1469598103934665603ull;
    auto mix = [&](uint32_t value) {
        key = (key ^ value) * 1099511628211ull;
    };
    uint32_t lodBiasBits = 0;
    std::memcpy(&lodBiasBits, &binding.mip_lod_bias, sizeof(lodBiasBits));
    mix(binding.address_u);
    mix(binding.address_v);
    mix(binding.address_w);
    mix(binding.min_filter);
    mix(binding.mag_filter);
    mix(binding.mip_filter);
    mix(lodBiasBits);
    mix(binding.max_mip_level);
    mix(binding.max_anisotropy);
    mix(binding.border_color);
    return key;
}

static RecordedTextureBinding plume_recorded_texture_binding(
    const XgpuTextureBinding &binding)
{
    RecordedTextureBinding recorded = {};
    recorded.guest = binding.guest_ptr;
    recorded.width = binding.width;
    recorded.height = binding.height;
    recorded.depth = binding.depth;
    recorded.levels = binding.levels;
    recorded.dimensionality = binding.dimensionality;
    recorded.bytes = binding.bytes;
    recorded.format = binding.format;
    recorded.version = binding.version;
    recorded.cube = binding.cube;
    recorded.unnormalizedCoords = binding.unnormalized_coords;
    return recorded;
}

::plume::RenderSampler *PlumeDraw::samplerForBinding(
    PlumeContext &ctx, const XgpuSamplerBinding &binding, bool valid)
{
    if (!valid)
        return m_progSampler.get();
    const uint64_t key = plume_sampler_key(binding);
    auto it = m_progSamplers.find(key);
    if (it != m_progSamplers.end())
        return it->second.get();

    RenderSamplerDesc desc;
    desc.minFilter = binding.min_filter == XGPU_SAMPLER_FILTER_POINT
        ? RenderFilter::NEAREST : RenderFilter::LINEAR;
    desc.magFilter = binding.mag_filter == XGPU_SAMPLER_FILTER_POINT
        ? RenderFilter::NEAREST : RenderFilter::LINEAR;
    desc.mipmapMode = binding.mip_filter == XGPU_SAMPLER_FILTER_LINEAR
        ? RenderMipmapMode::LINEAR : RenderMipmapMode::NEAREST;
    desc.addressU = plume_texture_address(binding.address_u);
    desc.addressV = plume_texture_address(binding.address_v);
    desc.addressW = plume_texture_address(binding.address_w);
    desc.mipLODBias = binding.mip_lod_bias;
    desc.minLOD = (float)binding.max_mip_level;
    if (binding.mip_filter == XGPU_SAMPLER_FILTER_NONE)
        desc.maxLOD = desc.minLOD;
    desc.anisotropyEnabled =
        binding.min_filter == XGPU_SAMPLER_FILTER_ANISOTROPIC &&
        binding.max_anisotropy > 1;
    if (binding.max_anisotropy)
        desc.maxAnisotropy = binding.max_anisotropy;
    desc.borderColor = plume_border_color(binding.border_color);
    std::unique_ptr<RenderSampler> sampler = ctx.device()->createSampler(desc);
    if (!sampler)
        return nullptr;
    RenderSampler *raw = sampler.get();
    m_progSamplers.emplace(key, std::move(sampler));
    return raw;
}

void PlumeDraw::setSampler(const XgpuSamplerBinding &binding)
{
    if (binding.stage >= 4)
        return;
    m_curSamplerState[binding.stage] = binding;
    m_curSamplerStateValid[binding.stage] = 1;
}

/* Textured PSO: POSITION + optional diffuse@16 + TEXCOORD@uvOffset. */
::plume::RenderPipeline *PlumeDraw::texPso(PlumeContext &ctx, uint32_t stride,
                                           uint8_t topology, uint8_t hasDiffuse,
                                           uint8_t uvOffset,
                                           const XgpuPlumeRenderState &renderState,
                                           bool hasDepthAttachment)
{
    uint32_t baseKey = (stride << 10) | (uint32_t(uvOffset) << 4) |
                       (uint32_t(topology) << 1) | (hasDiffuse ? 1u : 0u);
    uint64_t key = plume_render_state_key(renderState) ^
                   (uint64_t(baseKey) * 0x9E3779B185EBCA87ull);
    key = (key * 1099511628211ull) ^ (hasDepthAttachment ? 1ull : 0ull);
    auto it = m_texPsos.find(key);
    if (it != m_texPsos.end())
        return it->second.get();

    RenderInputSlot slot(0, stride);
    RenderInputElement elems[3];
    uint32_t ne = 0;
    elems[ne++] = RenderInputElement("POSITION", 0, 0,
                                     RenderFormat::R32G32B32A32_FLOAT, 0, 0);
    if (hasDiffuse)
        elems[ne++] = RenderInputElement("COLOR", 0, 1,
                                         RenderFormat::R8G8B8A8_UNORM, 0, 16);
    elems[ne++] = RenderInputElement("TEXCOORD", 0, hasDiffuse ? 2 : 1,
                                     RenderFormat::R32G32_FLOAT, 0, uvOffset);
    RenderGraphicsPipelineDesc pd;
    pd.pipelineLayout = m_texLayout.get();
    pd.vertexShader = hasDiffuse ? m_texVSDiffuse.get() : m_texVS.get();
    pd.pixelShader = m_texPS.get();
    pd.inputSlots = &slot;
    pd.inputSlotsCount = 1;
    pd.inputElements = elems;
    pd.inputElementsCount = ne;
    pd.renderTargetFormat[0] = RenderFormat::B8G8R8A8_UNORM;
    pd.renderTargetCount = 1;
    pd.primitiveTopology = (RenderPrimitiveTopology)topology;
    plume_draw_apply_state(pd, renderState, hasDepthAttachment);

    XRECOMP_TRACY_ZONE_SCOPED("Plume Create Graphics Pipeline");
    uint64_t perf_pipeline_t0 = xgpu_plume_perf_begin();
    std::unique_ptr<RenderPipeline> pso =
        ctx.device()->createGraphicsPipeline(pd);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_PIPELINE, perf_pipeline_t0);
    RenderPipeline *raw = pso.get();
    m_texPsos.emplace(key, std::move(pso));
    return raw;
}

bool PlumeDraw::bindTextureIfCached(const XgpuTextureBinding &binding)
{
    if (binding.stage >= 4u || !binding.guest_ptr || !m_texReady)
        return false;

    const auto shadow = m_guestTextureShadow.find(binding.guest_ptr);
    if (shadow == m_guestTextureShadow.end())
        return false;
    const RecordedTextureBinding &recorded = shadow->second;

    XgpuTextureBinding cached = {};
    cached.width = recorded.width;
    cached.height = recorded.height;
    cached.depth = recorded.depth;
    cached.levels = recorded.levels;
    cached.dimensionality = recorded.dimensionality;
    cached.bytes = recorded.bytes;
    cached.format = recorded.format;
    cached.version = recorded.version;
    cached.cube = recorded.cube;
    cached.unnormalized_coords = recorded.unnormalizedCoords;
    if (!plumeTextureBindingMatchesCached(cached, binding))
        return false;

    m_curTextureStage[binding.stage] = recorded;
    m_curSurfaceStage[binding.stage] = 0;
    m_curSurfaceUnnormalized[binding.stage] = 0;
    return true;
}

void PlumeDraw::setTexture(uint32_t stage, uint32_t guest, const void *pixels,
                           uint32_t w, uint32_t h, uint32_t pitch, uint32_t bytes,
                           uint32_t format, uint64_t version, uint32_t depth,
                           uint32_t levels, uint32_t dimensionality, uint32_t cube,
                           uint32_t unnormalizedCoords)
{
    if (stage >= 4)
        return;
    m_curSurfaceUnnormalized[stage] = 0;
    if (!m_texReady)
        return;
    auto clear_stage = [&]() {
        m_curTextureStage[stage] = {};
        m_curSurfaceStage[stage] = 0;
    };
    if (!guest || !pixels || !w || !h || !depth || !levels || !bytes) {
        clear_stage();
        return;
    }

    RenderFormat pf;
    if (!plume_map_texfmt(format, pf)) { clear_stage(); return; }   /* unsupported */
    const uint32_t textureLevels = levels;
    if (format == 0x11u && depth == 1u && !cube)
        f2_dump_linear_r5g6b5(
            guest, pixels, w, h, pitch, bytes, version);
    if ((format == 0x06u || format == 0x12u || format == 0x1Eu) &&
        depth == 1u && !cube)
        f2_dump_linear_bgra8(
            guest, pixels, w, h, pitch, bytes, format, version);
    if (depth == 1u && !cube &&
        (format == 0x0Cu || format == 0x0Fu))
        f2_dump_dxt_texture(
            guest, pixels, w, h, bytes, format, version);

    XgpuTextureBinding requested = {};
    requested.stage = stage;
    requested.guest_ptr = guest;
    requested.width = w;
    requested.height = h;
    requested.depth = depth;
    requested.levels = textureLevels;
    requested.dimensionality = dimensionality;
    requested.bytes = bytes;
    requested.format = format;
    requested.version = version;
    requested.cube = cube;
    requested.unnormalized_coords = unnormalizedCoords;
    if (guest != kHostFrameGuest && bindTextureIfCached(requested))
        return;

    /* Gate 3: the guest snapshots the payload and latches value state
     * only. The render owner consumes the upload at replay
     * (consumeTextureUploads) and owns every RHI object plus the
     * versioned texture store. */
    const RecordedTextureBinding recorded =
        plume_recorded_texture_binding(requested);
    RecordedTextureUpload command;
    command.binding = recorded;
    command.pitch = pitch;
    const uint8_t *ownedSource = static_cast<const uint8_t *>(pixels);
    command.pixels.assign(ownedSource, ownedSource + bytes);
    m_rec.textureUploads.push_back(std::move(command));
    m_guestTextureShadow[guest] = recorded;
    for (uint32_t s = 0; s < 4; s++) {
        if (s == stage || m_curTextureStage[s].guest == guest)
            m_curTextureStage[s] = recorded;
    }
    m_curSurfaceStage[stage] = 0;
}

/* Render-owner half of one recorded texture upload: format conversion,
 * RHI creation, and publication into the versioned store. A failure
 * simply leaves the binding unresolvable, so affected draws sample the
 * white fallback exactly like an unsupported format. */
void PlumeDraw::uploadRecordedTexture(PlumeContext &ctx,
                                      RecordedTextureUpload &&upload,
                                      RenderCommandList *cmdList)
{
    const RecordedTextureBinding &rb = upload.binding;
    const uint32_t guest = rb.guest;
    const void *pixels = upload.pixels.data();
    const uint32_t w = rb.width;
    const uint32_t h = rb.height;
    const uint32_t pitch = upload.pitch;
    const uint32_t bytes = rb.bytes;
    const uint32_t format = rb.format;
    const uint64_t version = rb.version;
    const uint32_t depth = rb.depth;
    const uint32_t textureLevels = rb.levels;
    const uint32_t levels = rb.levels;
    const uint32_t dimensionality = rb.dimensionality;
    const uint32_t cube = rb.cube;
    const uint32_t unnormalizedCoords = rb.unnormalizedCoords;
    const bool hostFrame = guest == kHostFrameGuest;
    RenderFormat pf;
    if (!plume_map_texfmt(format, pf))
        return;
    auto *hostCurrent = hostFrame ? m_textures.current(guest) : nullptr;
    const bool reuseHostTexture = hostCurrent &&
        hostCurrent->resource.tex && hostCurrent->resource.view &&
        hostCurrent->resource.descSet && hostCurrent->resource.w == w &&
        hostCurrent->resource.h == h && hostCurrent->resource.d == depth &&
        hostCurrent->resource.levels == textureLevels &&
        hostCurrent->resource.dimension == dimensionality &&
        hostCurrent->resource.fmt == format &&
        hostCurrent->resource.cube == cube;
    PlumeTex fresh;
    PlumeTex &t = reuseHostTexture ? hostCurrent->resource : fresh;
    t.w = w; t.h = h; t.d = depth; t.levels = textureLevels;
    t.dimension = dimensionality; t.fmt = format; t.bytes = bytes;
    t.cube = cube;
    t.unnormalizedCoords = unnormalizedCoords;
    t.version = version;
    /* Deferred-failure semantics: nothing to clear on the guest; the
     * binding just never resolves. */
    auto clear_stage = [&]() {};
    const void *uploadPixels = pixels;
    uint32_t uploadPitch = pitch;
    uint32_t uploadBytes = bytes;
    std::vector<uint8_t> convertedStorage;
    std::vector<uint8_t> &converted =
        hostFrame ? t.hostConverted : convertedStorage;
    if (format == 0x11) {
        uint64_t sourceOffset = 0;
        uint64_t convertedBytes64 = 0;
        uint32_t mipWidth = w;
        uint32_t mipHeight = h;
        for (uint32_t level = 0; level < textureLevels; ++level) {
            const uint64_t sourcePitch =
                (level == 0 && pitch) ? pitch : (uint64_t)mipWidth * 2u;
            sourceOffset += sourcePitch * mipHeight;
            convertedBytes64 += (uint64_t)mipWidth * 4u * mipHeight;
            mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
            mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
        }
        if (sourceOffset > bytes || convertedBytes64 > UINT32_MAX) {
            clear_stage();
            return;
        }
        uploadBytes = (uint32_t)convertedBytes64;
        converted.resize(uploadBytes);
        const uint8_t *src = (const uint8_t *)pixels;
        sourceOffset = 0;
        size_t destinationOffset = 0;
        mipWidth = w;
        mipHeight = h;
        for (uint32_t level = 0; level < textureLevels; ++level) {
            const uint32_t sourcePitch =
                (level == 0 && pitch) ? pitch : mipWidth * 2u;
            const uint32_t destinationPitch = mipWidth * 4u;
            for (uint32_t y = 0; y < mipHeight; ++y) {
                const uint8_t *sourceRow = src + sourceOffset +
                    (size_t)y * sourcePitch;
                uint8_t *destinationRow = converted.data() +
                    destinationOffset + (size_t)y * destinationPitch;
                for (uint32_t x = 0; x < mipWidth; ++x) {
                    const uint16_t pixel = (uint16_t)(sourceRow[x * 2u] |
                        ((uint16_t)sourceRow[x * 2u + 1u] << 8));
                    const uint8_t r5 = (uint8_t)((pixel >> 11) & 0x1Fu);
                    const uint8_t g6 = (uint8_t)((pixel >> 5) & 0x3Fu);
                    const uint8_t b5 = (uint8_t)(pixel & 0x1Fu);
                    destinationRow[x * 4u + 0u] =
                        (uint8_t)((b5 << 3) | (b5 >> 2));
                    destinationRow[x * 4u + 1u] =
                        (uint8_t)((g6 << 2) | (g6 >> 4));
                    destinationRow[x * 4u + 2u] =
                        (uint8_t)((r5 << 3) | (r5 >> 2));
                    destinationRow[x * 4u + 3u] = 0xFFu;
                }
            }
            sourceOffset += (uint64_t)sourcePitch * mipHeight;
            destinationOffset += (size_t)destinationPitch * mipHeight;
            mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
            mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
        }
        uploadPitch = textureLevels == 1 ? w * 4u : 0;
        uploadBytes = (uint32_t)convertedBytes64;
        uploadPixels = converted.data();
    } else if (format == 0x24u || format == 0x25u) {
        /* D3DFMT_YUY2/UYVY are the Xbox's 16-bit video formats.  Keep the
         * conversion identical to NV2A's native texture path: util.h owns
         * the XDK/xemu BT.601 integer coefficients and byte ordering. */
        uint64_t sourceOffset = 0;
        uint64_t convertedBytes64 = 0;
        uint32_t mipWidth = w;
        uint32_t mipHeight = h;
        for (uint32_t level = 0; level < textureLevels; ++level) {
            const uint64_t sourcePitch =
                (level == 0 && pitch) ? pitch : (uint64_t)mipWidth * 2u;
            if (!mipWidth || (mipWidth & 1u) ||
                sourcePitch < (uint64_t)mipWidth * 2u) {
                clear_stage();
                return;
            }
            sourceOffset += sourcePitch * mipHeight;
            convertedBytes64 += (uint64_t)mipWidth * 4u * mipHeight;
            mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
            mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
        }
        if (sourceOffset > bytes || convertedBytes64 > UINT32_MAX) {
            clear_stage();
            return;
        }
        uploadBytes = (uint32_t)convertedBytes64;
        converted.resize(uploadBytes);
        const uint8_t *src = (const uint8_t *)pixels;
        sourceOffset = 0;
        size_t destinationOffset = 0;
        mipWidth = w;
        mipHeight = h;
        for (uint32_t level = 0; level < textureLevels; ++level) {
            const uint32_t sourcePitch =
                (level == 0 && pitch) ? pitch : mipWidth * 2u;
            const uint32_t destinationPitch = mipWidth * 4u;
            for (uint32_t y = 0; y < mipHeight; ++y) {
                const uint8_t *sourceRow = src + sourceOffset +
                    (size_t)y * sourcePitch;
                uint8_t *destinationRow = converted.data() +
                    destinationOffset + (size_t)y * destinationPitch;
                for (uint32_t x = 0; x < mipWidth; ++x) {
                    uint8_t r;
                    uint8_t g;
                    uint8_t b;
                    if (format == 0x24u)
                        convert_yuy2_to_rgb(sourceRow, x, &r, &g, &b);
                    else
                        convert_uyvy_to_rgb(sourceRow, x, &r, &g, &b);
                    destinationRow[x * 4u + 0u] = b;
                    destinationRow[x * 4u + 1u] = g;
                    destinationRow[x * 4u + 2u] = r;
                    destinationRow[x * 4u + 3u] = 0xFFu;
                }
            }
            sourceOffset += (uint64_t)sourcePitch * mipHeight;
            destinationOffset += (size_t)destinationPitch * mipHeight;
            mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
            mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
        }
        uploadPitch = textureLevels == 1 ? w * 4u : 0;
        uploadPixels = converted.data();
    } else if (plumeCopyX8UploadWithOpaqueAlpha(
                   format, pixels, bytes, converted)) {
        uploadPixels = converted.data();
    }

    if (cube) {
        /* NV2A cubemap: 6 consecutive faces (+X,-X,+Y,-Y,+Z,-Z), each face a
         * full mip chain, face stride rounded up to 128 bytes (xemu
         * NV2A_CUBEMAP_FACE_ALIGNMENT). A8 and A8L8 are captured uncompressed
         * light cubes; other uncompressed cube formats remain fail-closed. */
        struct TextureMip {
            uint32_t width, height, rows, sourcePitch;
            uint32_t rowWidth, rowPitch;
            size_t sourceOffset, uploadOffset;
        };
        std::vector<TextureMip> mips;
        const uint32_t blockW = RenderFormatBlockWidth(pf);
        const uint32_t formatSize = RenderFormatSize(pf);
        size_t faceSourceBytes = 0;
        size_t faceStagingBytes = 0;
        uint32_t mw = w, mh = h;

        if (dimensionality != 2 ||
            (blockW <= 1 && format != 0x19u && format != 0x1Au) ||
            textureLevels > 16) {
            clear_stage();
            return;
        }
        for (uint32_t level = 0; level < textureLevels; level++) {
            TextureMip mip = {};
            mip.width = mw;
            mip.height = mh;
            mip.rows = (mh + blockW - 1u) / blockW;
            mip.sourcePitch = ((mw + blockW - 1u) / blockW) * formatSize;
            mip.sourceOffset = faceSourceBytes;
            plume_upload_layout(pf, mw, &mip.rowWidth, &mip.rowPitch);
            faceStagingBytes = (faceStagingBytes + 511u) & ~size_t(511u);
            mip.uploadOffset = faceStagingBytes;
            faceSourceBytes += (size_t)mip.sourcePitch * mip.rows;
            faceStagingBytes += (size_t)mip.rowPitch * mip.rows;
            mips.push_back(mip);
            mw = mw > 1 ? mw / 2 : 1;
            mh = mh > 1 ? mh / 2 : 1;
        }
        const size_t faceStride = (faceSourceBytes + 127u) & ~size_t(127u);
        faceStagingBytes = (faceStagingBytes + 511u) & ~size_t(511u);
        if (faceStagingBytes == 0 || faceStride * 6u > bytes) {
            clear_stage();
            return;
        }

        t.tex = ctx.device()->createTexture(RenderTextureDesc::Texture(
            RenderTextureDimension::TEXTURE_2D, w, h, 1, textureLevels, 6, pf,
            RenderTextureFlag::CUBE));
        if (!t.tex) { clear_stage(); return; }
        RenderTextureViewDesc viewDesc =
            RenderTextureViewDesc::TextureCube(pf);
        if (format == 0x19u) {
            /* A8 stores alpha in R; color channels sample as one. */
            viewDesc.componentMapping = RenderComponentMapping(
                RenderSwizzle::ONE, RenderSwizzle::ONE,
                RenderSwizzle::ONE, RenderSwizzle::R);
        } else if (format == 0x1Au) {
            /* A8L8 stores L in R and A in G after unswizzling. */
            viewDesc.componentMapping = RenderComponentMapping(
                RenderSwizzle::R, RenderSwizzle::R,
                RenderSwizzle::R, RenderSwizzle::G);
        }
        t.view = t.tex->createTextureView(viewDesc);
        if (!t.view) { clear_stage(); return; }

        std::unique_ptr<RenderBuffer> staging = ctx.device()->createBuffer(
            RenderBufferDesc::UploadBuffer(faceStagingBytes * 6u));
        if (!staging) { clear_stage(); return; }
        uint8_t *dst = (uint8_t *)staging->map();
        if (!dst) { clear_stage(); return; }
        const uint8_t *src = (const uint8_t *)uploadPixels;
        std::memset(dst, 0, faceStagingBytes * 6u);
        for (uint32_t face = 0; face < 6u; face++) {
            for (const TextureMip &mip : mips) {
                const uint32_t copyPitch = mip.sourcePitch < mip.rowPitch
                    ? mip.sourcePitch : mip.rowPitch;
                for (uint32_t row = 0; row < mip.rows; row++) {
                    std::memcpy(dst + (size_t)face * faceStagingBytes +
                                    mip.uploadOffset + (size_t)row * mip.rowPitch,
                                src + (size_t)face * faceStride +
                                    mip.sourceOffset + (size_t)row * mip.sourcePitch,
                                copyPitch);
                }
            }
        }
        staging->unmap();

        RenderCommandList *up = ctx.uploadCmd();
        up->begin();
        up->barriers(RenderBarrierStage::COPY,
                     RenderTextureBarrier(t.tex.get(), RenderTextureLayout::COPY_DEST));
        for (uint32_t face = 0; face < 6u; face++) {
            for (uint32_t level = 0; level < mips.size(); level++) {
                const TextureMip &mip = mips[level];
                RenderTextureCopyLocation srcLoc =
                    RenderTextureCopyLocation::PlacedFootprint(
                        staging.get(), pf, mip.width, mip.height, 1,
                        mip.rowWidth,
                        (size_t)face * faceStagingBytes + mip.uploadOffset);
                RenderTextureCopyLocation dstLoc =
                    RenderTextureCopyLocation::Subresource(t.tex.get(), level, face);
                up->copyTextureRegion(dstLoc, srcLoc, 0, 0, 0, nullptr);
            }
        }
        up->barriers(RenderBarrierStage::GRAPHICS,
                     RenderTextureBarrier(t.tex.get(), RenderTextureLayout::SHADER_READ));
        up->end();
        const RenderCommandList *cl = up;
        ctx.queue()->executeCommandLists(&cl, 1, nullptr, 0, nullptr, 0, ctx.fence());
        uint64_t wait_t0 = xgpu_plume_wait_stats_begin();
        ctx.queue()->waitForCommandFence(ctx.fence());
        xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_TEX_UPLOAD, wait_t0);
    } else if (dimensionality >= 3) {
        struct VolumeMip {
            uint32_t width, height, depth, rowWidth, rowPitch, slicePitch;
            size_t sourceOffset, sourceBytes, uploadOffset;
        };
        std::vector<VolumeMip> mips;
        size_t sourceOffset = 0;
        size_t uploadSize = 0;
        uint32_t mw = w, mh = h, md = depth;

        if (format != 0x0F || levels > 16) {
            clear_stage();
            return;
        }
        pf = RenderFormat::R8G8B8A8_UNORM;
        for (uint32_t level = 0; level < levels; level++) {
            VolumeMip mip = {};
            mip.width = mw;
            mip.height = mh;
            mip.depth = md;
            mip.sourceOffset = sourceOffset;
            mip.sourceBytes = (size_t)((mw + 3u) / 4u) *
                ((mh + 3u) / 4u) * md * 16u;
            plume_upload_layout(pf, mw, &mip.rowWidth, &mip.rowPitch);
            mip.slicePitch = mip.rowPitch * mh;
            uploadSize = (uploadSize + 511u) & ~size_t(511u);
            mip.uploadOffset = uploadSize;
            uploadSize += (size_t)mip.slicePitch * md;
            sourceOffset += mip.sourceBytes;
            mips.push_back(mip);
            mw = mw > 1 ? mw / 2 : 1;
            mh = mh > 1 ? mh / 2 : 1;
            md = md > 1 ? md / 2 : 1;
        }
        if (sourceOffset > bytes || uploadSize == 0) {
            clear_stage();
            return;
        }

        t.tex = ctx.device()->createTexture(
            RenderTextureDesc::Texture3D(w, h, depth, levels, pf));
        if (!t.tex) { clear_stage(); return; }
        t.view = t.tex->createTextureView(RenderTextureViewDesc::Texture3D(pf));
        if (!t.view) { clear_stage(); return; }
        std::unique_ptr<RenderBuffer> staging = ctx.device()->createBuffer(
            RenderBufferDesc::UploadBuffer(uploadSize));
        if (!staging) { clear_stage(); return; }
        uint8_t *mapped = (uint8_t *)staging->map();
        if (!mapped) { clear_stage(); return; }
        std::memset(mapped, 0, uploadSize);
        for (const VolumeMip &mip : mips) {
            if (!decode_bc3_volume_level(
                    (const uint8_t *)pixels + mip.sourceOffset, mip.sourceBytes,
                    mip.width, mip.height, mip.depth,
                    mapped + mip.uploadOffset, mip.rowPitch, mip.slicePitch)) {
                staging->unmap();
                clear_stage();
                return;
            }
        }
        staging->unmap();

        RenderCommandList *up = ctx.uploadCmd();
        up->begin();
        up->barriers(RenderBarrierStage::COPY,
                     RenderTextureBarrier(t.tex.get(), RenderTextureLayout::COPY_DEST));
        for (uint32_t level = 0; level < mips.size(); level++) {
            const VolumeMip &mip = mips[level];
            RenderTextureCopyLocation srcLoc = RenderTextureCopyLocation::PlacedFootprint(
                staging.get(), pf, mip.width, mip.height, mip.depth,
                mip.rowWidth, mip.uploadOffset);
            RenderTextureCopyLocation dstLoc =
                RenderTextureCopyLocation::Subresource(t.tex.get(), level, 0);
            up->copyTextureRegion(dstLoc, srcLoc, 0, 0, 0, nullptr);
        }
        up->barriers(RenderBarrierStage::GRAPHICS,
                     RenderTextureBarrier(t.tex.get(), RenderTextureLayout::SHADER_READ));
        up->end();
        const RenderCommandList *cl = up;
        ctx.queue()->executeCommandLists(&cl, 1, nullptr, 0, nullptr, 0, ctx.fence());
        uint64_t wait_t0 = xgpu_plume_wait_stats_begin();
        ctx.queue()->waitForCommandFence(ctx.fence());
        xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_TEX_UPLOAD, wait_t0);
    } else {
        struct TextureMip {
            uint32_t width, height, rows, sourcePitch;
            uint32_t rowWidth, rowPitch;
            size_t sourceOffset, uploadOffset;
        };
        std::vector<TextureMip> mips;
        const uint32_t blockW = RenderFormatBlockWidth(pf);
        const uint32_t formatSize = RenderFormatSize(pf);
        size_t sourceOffset = 0;
        size_t stagingBytes = 0;
        uint32_t mw = w, mh = h;

        if (textureLevels > 16) { clear_stage(); return; }
        for (uint32_t level = 0; level < textureLevels; level++) {
            TextureMip mip = {};
            mip.width = mw;
            mip.height = mh;
            mip.rows = (mh + blockW - 1u) / blockW;
            mip.sourcePitch = ((mw + blockW - 1u) / blockW) * formatSize;
            if (textureLevels == 1 && uploadPitch)
                mip.sourcePitch = uploadPitch;
            mip.sourceOffset = sourceOffset;
            plume_upload_layout(pf, mw, &mip.rowWidth, &mip.rowPitch);
            stagingBytes = (stagingBytes + 511u) & ~size_t(511u);
            mip.uploadOffset = stagingBytes;
            sourceOffset += (size_t)mip.sourcePitch * mip.rows;
            stagingBytes += (size_t)mip.rowPitch * mip.rows;
            mips.push_back(mip);
            mw = mw > 1 ? mw / 2 : 1;
            mh = mh > 1 ? mh / 2 : 1;
        }
        if (sourceOffset > uploadBytes || stagingBytes == 0) {
            clear_stage();
            return;
        }
        if (!reuseHostTexture) {
            t.tex = ctx.device()->createTexture(
                RenderTextureDesc::Texture2D(w, h, textureLevels, pf));
            if (!t.tex) { clear_stage(); return; }
            RenderTextureViewDesc viewDesc =
                RenderTextureViewDesc::Texture2D(pf);
            if (format == 0x19 || format == 0x1F) {
                /* A8 / LIN_A8: alpha-only; RGB sample as 1.0. */
                viewDesc.componentMapping = RenderComponentMapping(
                    RenderSwizzle::ONE, RenderSwizzle::ONE,
                    RenderSwizzle::ONE, RenderSwizzle::R);
            } else if (format == 0x1A || format == 0x20) {
                /* A8L8 / LIN_A8L8: (L,L,L,A). */
                viewDesc.componentMapping = RenderComponentMapping(
                    RenderSwizzle::R, RenderSwizzle::R,
                    RenderSwizzle::R, RenderSwizzle::G);
            } else if (format == 0x35) {
                viewDesc.componentMapping = RenderComponentMapping(
                    RenderSwizzle::R, RenderSwizzle::R,
                    RenderSwizzle::R, RenderSwizzle::ONE);
            }
            t.view = t.tex->createTextureView(viewDesc);
            if (!t.view) { clear_stage(); return; }
        }

        std::unique_ptr<RenderBuffer> staging =
            hostFrame ? nullptr : ctx.device()->createBuffer(
                RenderBufferDesc::UploadBuffer(stagingBytes));
        if (hostFrame &&
            (!t.hostUpload || t.hostUploadBytes < stagingBytes)) {
            t.hostUpload = ctx.device()->createBuffer(
                RenderBufferDesc::UploadBuffer(stagingBytes));
            t.hostUploadBytes = t.hostUpload ? stagingBytes : 0;
        }
        RenderBuffer *stagingBuffer =
            hostFrame ? t.hostUpload.get() : staging.get();
        if (!stagingBuffer) { clear_stage(); return; }
        uint8_t *dst = (uint8_t *)stagingBuffer->map();
        if (!dst) { clear_stage(); return; }
        const uint8_t *src = (const uint8_t *)uploadPixels;
        std::memset(dst, 0, stagingBytes);
        for (const TextureMip &mip : mips) {
            const uint32_t copyPitch = mip.sourcePitch < mip.rowPitch
                ? mip.sourcePitch : mip.rowPitch;
            for (uint32_t row = 0; row < mip.rows; row++) {
                std::memcpy(dst + mip.uploadOffset +
                                (size_t)row * mip.rowPitch,
                            src + mip.sourceOffset +
                                (size_t)row * mip.sourcePitch,
                            copyPitch);
            }
        }
        stagingBuffer->unmap();

        RenderCommandList *up = hostFrame ? cmdList : ctx.uploadCmd();
        if (!up) { clear_stage(); return; }
        if (!hostFrame)
            up->begin();
        up->barriers(RenderBarrierStage::COPY,
                     RenderTextureBarrier(t.tex.get(), RenderTextureLayout::COPY_DEST));
        for (uint32_t level = 0; level < mips.size(); level++) {
            const TextureMip &mip = mips[level];
            RenderTextureCopyLocation srcLoc =
                RenderTextureCopyLocation::PlacedFootprint(
                    stagingBuffer, pf, mip.width, mip.height, 1,
                    mip.rowWidth, mip.uploadOffset);
            RenderTextureCopyLocation dstLoc =
                RenderTextureCopyLocation::Subresource(t.tex.get(), level, 0);
            up->copyTextureRegion(dstLoc, srcLoc, 0, 0, 0, nullptr);
        }
        up->barriers(RenderBarrierStage::GRAPHICS,
                     RenderTextureBarrier(t.tex.get(), RenderTextureLayout::SHADER_READ));
        if (!hostFrame) {
            up->end();
            const RenderCommandList *cl = up;
            ctx.queue()->executeCommandLists(
                &cl, 1, nullptr, 0, nullptr, 0, ctx.fence());
            uint64_t wait_t0 = xgpu_plume_wait_stats_begin();
            ctx.queue()->waitForCommandFence(ctx.fence());
            xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_TEX_UPLOAD, wait_t0);
        }
    }

    const RenderSampler *immutableTexSampler = m_texSampler.get();
    RenderDescriptorRange ranges[2] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1,
                              &immutableTexSampler),
    };
    if (!reuseHostTexture) {
        RenderDescriptorSetDesc dsd(ranges, 2);
        t.descSet = ctx.device()->createDescriptorSet(dsd);
        if (!t.descSet) { clear_stage(); return; }
        t.descSet->setTexture(
            0, t.tex.get(), RenderTextureLayout::SHADER_READ, t.view.get());
    }

    if (reuseHostTexture)
        hostCurrent->binding = rb;
    else
        (void)m_textures.replace(rb, std::move(fresh));
}

void PlumeDraw::consumeTextureUploads(PlumeContext &ctx,
                                      FrameRecording &recording,
                                      RenderCommandList *cmdList)
{
    for (RecordedTextureUpload &upload : recording.textureUploads)
        uploadRecordedTexture(ctx, std::move(upload), cmdList);
    recording.textureUploads.clear();
}

bool PlumeDraw::ensureColorSurface(PlumeContext &ctx, uint64_t generation,
                                   uint32_t width, uint32_t height)
{
    if (m_surfaceCache.find(generation) != m_surfaceCache.end())
        return true;

    PlumeColorSurface surface;
    surface.width = width;
    surface.height = height;
    if (!plumeScaledExtent(width, height, m_internalResolutionScale,
                           &surface.physicalWidth,
                           &surface.physicalHeight))
        return false;
    surface.texture = ctx.device()->createTexture(
        RenderTextureDesc::ColorTarget(surface.physicalWidth,
                                       surface.physicalHeight,
                                       RenderFormat::B8G8R8A8_UNORM));
    surface.snapshot = ctx.device()->createTexture(
        RenderTextureDesc::ColorTarget(surface.physicalWidth,
                                       surface.physicalHeight,
                                       RenderFormat::B8G8R8A8_UNORM));
    if (!surface.texture || !surface.snapshot)
        return false;

    surface.view = surface.texture->createTextureView(
        RenderTextureViewDesc::Texture2D(RenderFormat::B8G8R8A8_UNORM));
    surface.snapshotView = surface.snapshot->createTextureView(
        RenderTextureViewDesc::Texture2D(RenderFormat::B8G8R8A8_UNORM));
    const RenderSampler *immutableTexSampler = m_texSampler.get();
    RenderDescriptorRange ranges[2] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1,
                              &immutableTexSampler),
    };
    RenderDescriptorSetDesc descriptorDesc(ranges, 2);
    surface.descSet = ctx.device()->createDescriptorSet(descriptorDesc);
    surface.snapshotDescSet = ctx.device()->createDescriptorSet(descriptorDesc);
    if (!surface.view || !surface.snapshotView || !surface.descSet ||
        !surface.snapshotDescSet)
        return false;

    surface.layout = RenderTextureLayout::UNKNOWN;
    surface.snapshotLayout = RenderTextureLayout::UNKNOWN;
    surface.descSet->setTexture(0, surface.texture.get(),
                                RenderTextureLayout::SHADER_READ,
                                surface.view.get());
    surface.snapshotDescSet->setTexture(0, surface.snapshot.get(),
                                        RenderTextureLayout::SHADER_READ,
                                        surface.snapshotView.get());
    m_surfaceCache.emplace(generation, std::move(surface));
    return true;
}

bool PlumeDraw::ensureBackbufferMirror(PlumeContext &ctx, uint32_t guest)
{
    /*
     * The device backbuffer renders through the screen path, so no surface
     * generation is ever registered for its key.  A texture aliasing that
     * memory must still bind the surface path: the version-keyed upload
     * cache can never track GPU-rendered memory (bug-850).  Mint a mirror
     * surface on demand; the replay loops recognize guestAddr ==
     * m_presentTarget and refresh it from the live screen texture before
     * every sampling draw.  The high-bit generation id cannot collide with
     * tracker-issued generations, and a later real registration of the
     * same guest key simply supersedes this entry in
     * m_latestSurfaceGeneration.
     */
    if (!guest || guest != m_presentTarget)
        return false;
    if (m_latestSurfaceGeneration.find(guest) !=
        m_latestSurfaceGeneration.end())
        return true;
    const uint64_t generation = backbufferMirrorGeneration(guest);
    if (!ensureColorSurface(ctx, generation, m_outputWidth, m_outputHeight))
        return false;
    auto it = m_surfaceCache.find(generation);
    if (it == m_surfaceCache.end())
        return false;
    it->second.guestAddr = guest;
    m_latestSurfaceGeneration[guest] = generation;
    m_guestLatestSurfaceGeneration[guest] = generation;
    m_guestSurfaceAddress[generation] = guest;
    return true;
}

/* Resolve a rendered depth surface into guest memory as packed Z24S8.
 *
 * Only plane 0 (depth) is reachable: RenderTextureCopyLocation::Subresource
 * has no plane parameter, so the stencil plane cannot be copied and is written
 * as zero. NV2A packs Z24S8 as (depth24 << 8) | stencil, so the stencil byte is
 * the low byte of each dword.
 *
 * D3D12 permits copying plane 0 of a depth resource into a buffer with an
 * R32_FLOAT footprint; Vulkan's toAspectFlags() yields depth-only for a
 * DEPTH_TARGET, which is what vkCmdCopyImageToBuffer requires. Sampling the
 * depth texture directly would need an R32G8X24_TYPELESS resource, which Plume
 * does not create -- hence the round trip through guest memory. */
void PlumeDraw::noteZetaWrite(uint64_t zetaGeneration)
{
    if (!zetaGeneration)
        return;
    auto it = m_zetaCache.find(zetaGeneration);
    if (it != m_zetaCache.end())
        ++it->second.contentSerial;
}

void PlumeDraw::noteColorWrite(uint64_t colorGeneration)
{
    if (!colorGeneration)
        return;
    auto it = m_surfaceCache.find(colorGeneration);
    if (it != m_surfaceCache.end())
        ++it->second.contentSerial;
}

bool PlumeDraw::colorSurfacePendingCpuSync(uint32_t guest) const
{
    auto recordedLatest = m_guestLatestSurfaceGeneration.find(guest);
    if (recordedLatest == m_guestLatestSurfaceGeneration.end())
        return true; /* unknown surface: preserve the caller's drain */
    for (const GeomDraw &draw : m_rec.draws) {
        if (draw.recordsColorWrite &&
            draw.targetColorGeneration == recordedLatest->second)
            return true;
    }
    for (const ProgDraw &draw : m_rec.progDraws) {
        if (draw.recordsColorWrite &&
            draw.targetColorGeneration == recordedLatest->second)
            return true;
    }
    auto latest = m_latestSurfaceGeneration.find(guest);
    if (latest == m_latestSurfaceGeneration.end() ||
        latest->second != recordedLatest->second)
        return true; /* recorded generation has not reached the owner yet */
    auto it = m_surfaceCache.find(latest->second);
    if (it == m_surfaceCache.end())
        return true;
    return it->second.resolvedSerial != it->second.contentSerial;
}

bool PlumeDraw::zetaSurfaceNeedsDownload(uint32_t guest, uint32_t width,
                                         uint32_t height,
                                         uint32_t pitch) const
{
    if (!width || !height || width > (UINT32_MAX - 255u) / 4u ||
        pitch < width * 4u)
        return false;
    auto recordedLatest = m_guestLatestZetaGeneration.find(guest);
    if (recordedLatest != m_guestLatestZetaGeneration.end()) {
        for (const GeomDraw &draw : m_rec.draws) {
            if (draw.recordsZetaWrite &&
                draw.targetZetaGeneration == recordedLatest->second)
                return true;
        }
        for (const ProgDraw &draw : m_rec.progDraws) {
            if (draw.recordsZetaWrite &&
                draw.targetZetaGeneration == recordedLatest->second)
                return true;
        }
    }
    auto latest = m_latestZetaGeneration.find(guest);
    if (latest == m_latestZetaGeneration.end())
        return false;
    auto it = m_zetaCache.find(latest->second);
    if (it == m_zetaCache.end() || it->second.width != width ||
        it->second.height != height ||
        it->second.layout == RenderTextureLayout::UNKNOWN)
        return false;
    return it->second.downloadedSerial != it->second.contentSerial;
}

bool PlumeDraw::downloadZetaSurface(PlumeContext &ctx, uint32_t guest,
                                    void *pixels, uint32_t width,
                                    uint32_t height, uint32_t pitch)
{
    auto latest = m_latestZetaGeneration.find(guest);
    if (!pixels || !zetaSurfaceNeedsDownload(
                       guest, width, height, pitch))
        return false;
    auto it = m_zetaCache.find(latest->second);
    if (it == m_zetaCache.end() || it->second.width != width ||
        it->second.height != height ||
        it->second.layout == RenderTextureLayout::UNKNOWN)
        return false;

    PlumeZetaSurface &surface = it->second;
    /* Guest memory already holds these depth bytes: no depth-writing work
     * was recorded since the last resolve, so skip the readback stall and
     * report "not refreshed" — the caller then keeps its texture version
     * and the cached host texture, avoiding the re-upload as well. */
    if (surface.downloadedSerial == surface.contentSerial)
        return false;
    if (surface.physicalWidth > (UINT32_MAX - 255u) / 4u)
        return false;
    const uint32_t readbackPitch =
        (surface.physicalWidth * 4u + 255u) & ~255u;
    std::unique_ptr<RenderBuffer> readback = ctx.device()->createBuffer(
        RenderBufferDesc::ReadbackBuffer(
            (uint64_t)readbackPitch * surface.physicalHeight));
    if (!readback)
        return false;

    const RenderTextureLayout previous = surface.layout;
    RenderCommandList *command = ctx.uploadCmd();
    command->begin();
    if (previous != RenderTextureLayout::COPY_SOURCE)
        command->barriers(RenderBarrierStage::COPY,
                          RenderTextureBarrier(
                              surface.texture.get(),
                              RenderTextureLayout::COPY_SOURCE));
    const RenderTextureCopyLocation destination =
        RenderTextureCopyLocation::PlacedFootprint(
            readback.get(), RenderFormat::R32_FLOAT,
            surface.physicalWidth, surface.physicalHeight,
            1, readbackPitch / 4u);
    const RenderTextureCopyLocation source =
        RenderTextureCopyLocation::Subresource(surface.texture.get(), 0, 0);
    command->copyTextureRegion(destination, source);
    if (previous != RenderTextureLayout::COPY_SOURCE)
        command->barriers(previous == RenderTextureLayout::COPY_DEST
                              ? RenderBarrierStage::COPY
                              : RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(surface.texture.get(),
                                               previous));
    command->end();
    const RenderCommandList *submitted = command;
    ctx.queue()->executeCommandLists(&submitted, 1, nullptr, 0, nullptr, 0,
                                     ctx.fence());
    uint64_t wait_t0 = xgpu_plume_wait_stats_begin();
    ctx.queue()->waitForCommandFence(ctx.fence());
    xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_ZETA_DOWNLOAD, wait_t0);

    const RenderRange range(
        0, (uint64_t)readbackPitch * surface.physicalHeight);
    const uint8_t *mapped = static_cast<const uint8_t *>(
        readback->map(0, &range));
    if (!mapped)
        return false;
    std::vector<float> logicalDepth((size_t)width * height);
    if (!plumeDownscaleDepthR32Nearest(
            mapped, surface.physicalWidth, surface.physicalHeight,
            readbackPitch, logicalDepth.data(), width, height,
            width * sizeof(float))) {
        readback->unmap();
        return false;
    }
    for (uint32_t row = 0; row < height; ++row) {
        const float *src = logicalDepth.data() + (size_t)row * width;
        uint8_t *dstRow = static_cast<uint8_t *>(pixels) + (size_t)row * pitch;
        for (uint32_t x = 0; x < width; ++x) {
            float d = src[x];
            d = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
            /* Clamp the integer, not just the float: 1.0f * 16777215.0f + 0.5f
             * is not representable in a 24-bit mantissa and rounds UP to
             * 0x1000000, whose << 8 overflows uint32 to zero -- turning every
             * far-plane pixel into 0. */
            uint32_t z24 = (uint32_t)(d * 16777215.0f + 0.5f);
            if (z24 > 0xFFFFFFu)
                z24 = 0xFFFFFFu;
            const uint32_t packed = z24 << 8;
            std::memcpy(dstRow + (size_t)x * 4u, &packed, sizeof(packed));
        }
    }
    readback->unmap();
    surface.downloadedSerial = surface.contentSerial;
    return true;
}

/* GPU zeta->Y16 alias conversion (plume-gpu-zeta-alias-conversion.md).
 * Lane arithmetic matches plume_zeta_alias.h and the CPU pack above;
 * tests/plume_zeta_alias_pack_test.cpp holds the shared vectors. */
bool PlumeDraw::ensureZetaAliasPipeline(PlumeContext &ctx)
{
    if (m_zetaAliasReady)
        return true;
    static const char *kZetaAliasHlsl =
        "struct ZetaAliasPC { uint logicalW; uint logicalH;"
        " uint pad0; uint pad1; };\n"
        "#ifdef XGPU_SPIRV\n"
        "[[vk::push_constant]] ConstantBuffer<ZetaAliasPC> aliasPC;\n"
        "#else\n"
        "ConstantBuffer<ZetaAliasPC> aliasPC : register(b0);\n"
        "#endif\n"
        "Texture2D<float> depthSource : register(t0);\n"
        "SamplerState pointSampler : register(s1);\n"
        "struct VSOut { float4 position : SV_Position; };\n"
        "VSOut VSMain(uint id : SV_VertexID) {\n"
        "  float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  VSOut o;\n"
        "  o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0),"
        " 0.0, 1.0);\n"
        "  return o;\n"
        "}\n"
        "float4 PSMain(VSOut i) : SV_Target {\n"
        "  uint x = uint(i.position.x);\n"
        "  uint y = uint(i.position.y);\n"
        "  uint sx = x >> 1;\n"
        "  float2 uv = float2((float(sx) + 0.5) / float(aliasPC.logicalW),\n"
        "                     (float(y) + 0.5) / float(aliasPC.logicalH));\n"
        "  float d = saturate(depthSource.SampleLevel(pointSampler, uv, 0));\n"
        "  uint z24 = min(uint(d * 16777215.0 + 0.5), 0xFFFFFFu);\n"
        "  uint dw = z24 << 8;\n"
        "  uint lane = (x & 1u) != 0u ? (dw >> 16) : (dw & 0xFFFFu);\n"
        "  return float4(float(lane) / 65535.0, 0.0, 0.0, 1.0);\n"
        "}\n";
    ShaderCompileResult vs = compileForContext(
        ctx, kZetaAliasHlsl, "VSMain", "vs_6_0");
    ShaderCompileResult ps = compileForContext(
        ctx, kZetaAliasHlsl, "PSMain", "ps_6_0");
    if (!vs.ok || !ps.ok || vs.target != ps.target)
        return false;
    const RenderShaderFormat format = renderShaderFormatForTarget(vs.target);
    m_zetaAliasVS = ctx.device()->createShader(
        vs.bytecode.data(), vs.bytecode.size(), vs.entryPoint.c_str(),
        format);
    m_zetaAliasPS = ctx.device()->createShader(
        ps.bytecode.data(), ps.bytecode.size(), ps.entryPoint.c_str(),
        format);
    RenderSamplerDesc samplerDesc;
    samplerDesc.minFilter = RenderFilter::NEAREST;
    samplerDesc.magFilter = RenderFilter::NEAREST;
    samplerDesc.mipmapMode = RenderMipmapMode::NEAREST;
    samplerDesc.addressU = RenderTextureAddressMode::CLAMP;
    samplerDesc.addressV = RenderTextureAddressMode::CLAMP;
    samplerDesc.addressW = RenderTextureAddressMode::CLAMP;
    m_zetaAliasSampler = ctx.device()->createSampler(samplerDesc);
    const RenderSampler *immutableSampler = m_zetaAliasSampler.get();
    RenderDescriptorRange ranges[2] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1,
                              &immutableSampler),
    };
    RenderDescriptorSetDesc descriptorDesc(ranges, 2);
    RenderPushConstantRange aliasPC(
        0, 0, 0, 4u * sizeof(uint32_t), RenderShaderStageFlag::PIXEL);
    RenderPipelineLayoutDesc layoutDesc;
    layoutDesc.descriptorSetDescs = &descriptorDesc;
    layoutDesc.descriptorSetDescsCount = 1;
    layoutDesc.pushConstantRanges = &aliasPC;
    layoutDesc.pushConstantRangesCount = 1;
    m_zetaAliasLayout = ctx.device()->createPipelineLayout(layoutDesc);
    RenderGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = m_zetaAliasLayout.get();
    pipelineDesc.vertexShader = m_zetaAliasVS.get();
    pipelineDesc.pixelShader = m_zetaAliasPS.get();
    pipelineDesc.renderTargetFormat[0] = RenderFormat::R16_UNORM;
    pipelineDesc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    pipelineDesc.renderTargetCount = 1;
    pipelineDesc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    m_zetaAliasPso = ctx.device()->createGraphicsPipeline(pipelineDesc);
    m_zetaAliasReady = m_zetaAliasVS && m_zetaAliasPS &&
                       m_zetaAliasSampler && m_zetaAliasLayout &&
                       m_zetaAliasPso;
    return m_zetaAliasReady;
}

bool PlumeDraw::bindZetaAliasStage(PlumeContext &ctx, uint32_t stage,
                                   uint32_t zetaGuest, uint32_t aliasWidth,
                                   uint32_t aliasHeight, uint32_t aliasPitch,
                                   uint32_t unnormalizedCoords)
{
    if (stage >= 4u || !m_texReady || !aliasWidth || !aliasHeight)
        return false;
    auto latest = m_latestZetaGeneration.find(zetaGuest);
    if (latest == m_latestZetaGeneration.end())
        return false;
    auto it = m_zetaCache.find(latest->second);
    if (it == m_zetaCache.end())
        return false;
    PlumeZetaSurface &zeta = it->second;
    if (!zeta.texture || zeta.layout == RenderTextureLayout::UNKNOWN ||
        aliasWidth != zeta.width * 2u || aliasHeight != zeta.height)
        return false;
    if (aliasPitch < aliasWidth * 2u)
        return false;

    RecordedTextureBinding binding;
    binding.guest = zetaGuest;
    binding.width = aliasWidth;
    binding.height = aliasHeight;
    binding.depth = 1;
    binding.levels = 1;
    binding.dimensionality = 2;
    binding.bytes = aliasPitch * aliasHeight;
    binding.format = 0x35u;
    binding.version = zeta.contentSerial;
    binding.cube = 0;
    binding.unnormalizedCoords = unnormalizedCoords ? 1u : 0u;

    if (zeta.convertedSerial == zeta.contentSerial &&
        m_textures.resolve(binding)) {
        m_curTextureStage[stage] = binding;
        m_curSurfaceStage[stage] = 0;
        m_curSurfaceUnnormalized[stage] = 0;
        return true;
    }
    if (!ensureZetaAliasPipeline(ctx))
        return false;

    /* Staging chain: depth plane 0 -> buffer -> R32 texture. */
    const uint32_t physW = zeta.physicalWidth ? zeta.physicalWidth
                                              : zeta.width;
    const uint32_t physH = zeta.physicalHeight ? zeta.physicalHeight
                                               : zeta.height;
    const uint32_t stagePitch = (physW * 4u + 255u) & ~255u;
    if (!zeta.convertScratch)
        zeta.convertScratch = ctx.device()->createBuffer(
            RenderBufferDesc::DefaultBuffer((uint64_t)stagePitch * physH));
    if (!zeta.convertSource)
        zeta.convertSource = ctx.device()->createTexture(
            RenderTextureDesc::Texture2D(physW, physH, 1,
                                         RenderFormat::R32_FLOAT));
    if (!zeta.convertScratch || !zeta.convertSource)
        return false;

    PlumeTex out;
    out.w = aliasWidth; out.h = aliasHeight; out.d = 1; out.levels = 1;
    out.dimension = 2; out.fmt = 0x35u; out.bytes = binding.bytes;
    out.cube = 0; out.unnormalizedCoords = binding.unnormalizedCoords;
    out.version = binding.version;
    out.tex = ctx.device()->createTexture(RenderTextureDesc::ColorTarget(
        aliasWidth, aliasHeight, RenderFormat::R16_UNORM));
    if (!out.tex)
        return false;
    out.view = out.tex->createTextureView(
        RenderTextureViewDesc::Texture2D(RenderFormat::R16_UNORM));
    if (!out.view)
        return false;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    {
        const RenderTexture *color = out.tex.get();
        RenderFramebufferDesc fbDesc;
        fbDesc.colorAttachments = &color;
        fbDesc.colorAttachmentsCount = 1;
        framebuffer = ctx.device()->createFramebuffer(fbDesc);
    }
    std::unique_ptr<RenderDescriptorSet> passSet;
    {
        const RenderSampler *immutableSampler = m_zetaAliasSampler.get();
        RenderDescriptorRange ranges[2] = {
            RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
            RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1,
                                  &immutableSampler),
        };
        RenderDescriptorSetDesc dsd(ranges, 2);
        passSet = ctx.device()->createDescriptorSet(dsd);
    }
    if (!framebuffer || !passSet)
        return false;

    const RenderTextureLayout zetaPrevious = zeta.layout;
    RenderCommandList *cmd = ctx.uploadCmd();
    cmd->begin();
    if (zetaPrevious != RenderTextureLayout::COPY_SOURCE)
        cmd->barriers(RenderBarrierStage::COPY,
                      RenderTextureBarrier(zeta.texture.get(),
                                           RenderTextureLayout::COPY_SOURCE));
    const RenderTextureCopyLocation bufferDst =
        RenderTextureCopyLocation::PlacedFootprint(
            zeta.convertScratch.get(), RenderFormat::R32_FLOAT,
            physW, physH, 1, stagePitch / 4u);
    const RenderTextureCopyLocation depthSrc =
        RenderTextureCopyLocation::Subresource(zeta.texture.get(), 0, 0);
    cmd->copyTextureRegion(bufferDst, depthSrc);
    cmd->barriers(RenderBarrierStage::COPY,
                  RenderTextureBarrier(zeta.convertSource.get(),
                                       RenderTextureLayout::COPY_DEST));
    const RenderTextureCopyLocation textureDst =
        RenderTextureCopyLocation::Subresource(zeta.convertSource.get(),
                                               0, 0);
    const RenderTextureCopyLocation bufferSrc =
        RenderTextureCopyLocation::PlacedFootprint(
            zeta.convertScratch.get(), RenderFormat::R32_FLOAT,
            physW, physH, 1, stagePitch / 4u);
    cmd->copyTextureRegion(textureDst, bufferSrc);
    cmd->barriers(RenderBarrierStage::GRAPHICS,
                  RenderTextureBarrier(zeta.convertSource.get(),
                                       RenderTextureLayout::SHADER_READ));
    cmd->barriers(RenderBarrierStage::GRAPHICS,
                  RenderTextureBarrier(out.tex.get(),
                                       RenderTextureLayout::COLOR_WRITE));
    passSet->setTexture(0, zeta.convertSource.get(),
                        RenderTextureLayout::SHADER_READ);
    cmd->setPipeline(m_zetaAliasPso.get());
    cmd->setGraphicsPipelineLayout(m_zetaAliasLayout.get());
    cmd->setGraphicsDescriptorSet(passSet.get(), 0);
    const uint32_t aliasConstants[4] = {zeta.width, zeta.height, 0, 0};
    cmd->setGraphicsPushConstants(0, aliasConstants);
    cmd->setFramebuffer(framebuffer.get());
    cmd->setViewports(RenderViewport(0.0f, 0.0f, float(aliasWidth),
                                     float(aliasHeight)));
    cmd->setScissors(RenderRect(0, 0, aliasWidth, aliasHeight));
    cmd->drawInstanced(3, 1, 0, 0);
    cmd->barriers(RenderBarrierStage::GRAPHICS,
                  RenderTextureBarrier(out.tex.get(),
                                       RenderTextureLayout::SHADER_READ));
    if (zetaPrevious != RenderTextureLayout::COPY_SOURCE)
        cmd->barriers(zetaPrevious == RenderTextureLayout::COPY_DEST
                          ? RenderBarrierStage::COPY
                          : RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(zeta.texture.get(),
                                           zetaPrevious));
    cmd->end();
    const RenderCommandList *submitted = cmd;
    ctx.queue()->executeCommandLists(&submitted, 1, nullptr, 0, nullptr, 0,
                                     ctx.fence());
    uint64_t wait_t0 = xgpu_plume_wait_stats_begin();
    ctx.queue()->waitForCommandFence(ctx.fence());
    xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_ZETA_DOWNLOAD, wait_t0);

    const RenderSampler *immutableTexSampler = m_texSampler.get();
    RenderDescriptorRange texRanges[2] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1,
                              &immutableTexSampler),
    };
    RenderDescriptorSetDesc texDsd(texRanges, 2);
    out.descSet = ctx.device()->createDescriptorSet(texDsd);
    if (!out.descSet)
        return false;
    out.descSet->setTexture(0, out.tex.get(),
                            RenderTextureLayout::SHADER_READ,
                            out.view.get());

    (void)m_textures.replace(binding, std::move(out));
    zeta.convertedSerial = zeta.contentSerial;
    m_curTextureStage[stage] = binding;
    m_curSurfaceStage[stage] = 0;
    m_curSurfaceUnnormalized[stage] = 0;
    return true;
}

bool PlumeDraw::downloadColorSurface(PlumeContext &ctx, uint32_t guest,
                                     void *pixels, uint32_t width,
                                     uint32_t height, uint32_t pitch)
{
    auto latest = m_latestSurfaceGeneration.find(guest);
    if (!pixels || !width || !height ||
        width > (UINT32_MAX - 255u) / 4u ||
        pitch < width * 4u || latest == m_latestSurfaceGeneration.end())
        return false;
    auto it = m_surfaceCache.find(latest->second);
    if (it == m_surfaceCache.end() || it->second.width != width ||
        it->second.height != height ||
        it->second.layout == RenderTextureLayout::UNKNOWN)
        return false;

    PlumeColorSurface &surface = it->second;
    if (surface.physicalWidth > (UINT32_MAX - 255u) / 4u)
        return false;
    const uint32_t readbackPitch =
        (surface.physicalWidth * 4u + 255u) & ~255u;
    std::unique_ptr<RenderBuffer> readback = ctx.device()->createBuffer(
        RenderBufferDesc::ReadbackBuffer(
            (uint64_t)readbackPitch * surface.physicalHeight));
    if (!readback)
        return false;

    const RenderTextureLayout previous = surface.layout;
    RenderCommandList *command = ctx.uploadCmd();
    command->begin();
    if (previous != RenderTextureLayout::COPY_SOURCE)
        command->barriers(RenderBarrierStage::COPY,
                          RenderTextureBarrier(
                              surface.texture.get(),
                              RenderTextureLayout::COPY_SOURCE));
    const RenderTextureCopyLocation destination =
        RenderTextureCopyLocation::PlacedFootprint(
            readback.get(), RenderFormat::B8G8R8A8_UNORM,
            surface.physicalWidth, surface.physicalHeight,
            1, readbackPitch / 4u);
    const RenderTextureCopyLocation source =
        RenderTextureCopyLocation::Subresource(surface.texture.get(), 0, 0);
    command->copyTextureRegion(destination, source);
    if (previous != RenderTextureLayout::COPY_SOURCE)
        command->barriers(previous == RenderTextureLayout::COPY_DEST
                              ? RenderBarrierStage::COPY
                              : RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(surface.texture.get(),
                                               previous));
    command->end();
    const RenderCommandList *submitted = command;
    ctx.queue()->executeCommandLists(&submitted, 1, nullptr, 0, nullptr, 0,
                                     ctx.fence());
    uint64_t wait_t0 = xgpu_plume_wait_stats_begin();
    ctx.queue()->waitForCommandFence(ctx.fence());
    xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_SURFACE_DOWNLOAD, wait_t0);

    const RenderRange range(
        0, (uint64_t)readbackPitch * surface.physicalHeight);
    const uint8_t *mapped = static_cast<const uint8_t *>(
        readback->map(0, &range));
    if (!mapped)
        return false;
    const bool scaled = plumeScaleColorBgra8Linear(
        mapped, surface.physicalWidth, surface.physicalHeight,
        readbackPitch, static_cast<uint8_t *>(pixels), width, height, pitch);
    readback->unmap();
    if (scaled)
        surface.resolvedSerial = surface.contentSerial;
    return scaled;
}

bool PlumeDraw::uploadColorSurface(PlumeContext &ctx, uint32_t guest,
                                   const void *pixels, uint32_t width,
                                   uint32_t height, uint32_t pitch)
{
    auto latest = m_latestSurfaceGeneration.find(guest);
    if (!pixels || !width || !height ||
        width > (UINT32_MAX - 255u) / 4u ||
        pitch < width * 4u || latest == m_latestSurfaceGeneration.end())
        return false;
    auto it = m_surfaceCache.find(latest->second);
    if (it == m_surfaceCache.end() || it->second.width != width ||
        it->second.height != height)
        return false;

    PlumeColorSurface &surface = it->second;
    if (surface.physicalWidth > (UINT32_MAX - 255u) / 4u)
        return false;
    const uint32_t uploadPitch =
        (surface.physicalWidth * 4u + 255u) & ~255u;
    std::unique_ptr<RenderBuffer> upload = ctx.device()->createBuffer(
        RenderBufferDesc::UploadBuffer(
            (uint64_t)uploadPitch * surface.physicalHeight));
    if (!upload)
        return false;
    uint8_t *mapped = static_cast<uint8_t *>(upload->map());
    if (!mapped)
        return false;
    std::memset(mapped, 0,
                (size_t)uploadPitch * surface.physicalHeight);
    const bool scaled = plumeScaleColorBgra8Linear(
        static_cast<const uint8_t *>(pixels), width, height, pitch,
        mapped, surface.physicalWidth, surface.physicalHeight, uploadPitch);
    upload->unmap();
    if (!scaled)
        return false;

    RenderCommandList *command = ctx.uploadCmd();
    command->begin();
    command->barriers(RenderBarrierStage::COPY,
                      RenderTextureBarrier(surface.texture.get(),
                                           RenderTextureLayout::COPY_DEST));
    const RenderTextureCopyLocation source =
        RenderTextureCopyLocation::PlacedFootprint(
            upload.get(), RenderFormat::B8G8R8A8_UNORM,
            surface.physicalWidth, surface.physicalHeight,
            1, uploadPitch / 4u);
    const RenderTextureCopyLocation destination =
        RenderTextureCopyLocation::Subresource(surface.texture.get(), 0, 0);
    command->copyTextureRegion(destination, source);
    command->barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(surface.texture.get(),
                                           RenderTextureLayout::COLOR_WRITE));
    command->end();
    const RenderCommandList *submitted = command;
    ctx.queue()->executeCommandLists(&submitted, 1, nullptr, 0, nullptr, 0,
                                     ctx.fence());
    uint64_t wait_t0 = xgpu_plume_wait_stats_begin();
    ctx.queue()->waitForCommandFence(ctx.fence());
    xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_SURFACE_RESTORE, wait_t0);
    surface.layout = RenderTextureLayout::COLOR_WRITE;
    surface.resolvedSerial = surface.contentSerial;
    return true;
}

bool PlumeDraw::blitSurface(PlumeContext &ctx,
                            const XgpuSurfaceBinding &destination,
                            uint32_t dstGuest, uint32_t srcResource,
                            uint32_t srcGuest)
{
    if (!m_texReady || !destination.color_resource || !srcResource ||
        destination.color_resource > UINT32_MAX ||
        !destination.width || !destination.height)
        return false;
    const uint32_t dstResource =
        static_cast<uint32_t>(destination.color_resource);
    dstGuest = dstGuest ? dstGuest : dstResource;
    uint64_t srcGeneration = 0;
    auto recordedSource = m_guestLatestSurfaceGeneration.find(srcResource);
    if (recordedSource == m_guestLatestSurfaceGeneration.end() && srcGuest)
        recordedSource = m_guestLatestSurfaceGeneration.find(srcGuest);
    if (recordedSource != m_guestLatestSurfaceGeneration.end())
        srcGeneration = recordedSource->second;
    if (!srcGeneration) {
        auto liveSource = m_latestSurfaceGeneration.find(srcResource);
        if (liveSource == m_latestSurfaceGeneration.end() && srcGuest)
            liveSource = m_latestSurfaceGeneration.find(srcGuest);
        if (liveSource != m_latestSurfaceGeneration.end())
            srcGeneration = liveSource->second;
    }
    if (!srcGeneration)
        return false;
    auto srcIt = m_surfaceCache.find(srcGeneration);
    const uint32_t width = destination.image_width
        ? destination.image_width : destination.width;
    const uint32_t height = destination.image_height
        ? destination.image_height : destination.height;
    if (srcIt != m_surfaceCache.end() &&
        (srcIt->second.width != width || srcIt->second.height != height))
        return false;

    const PlumeSurfaceBindingIds ids =
        m_surfaceBindingTracker.bind(destination);
    if (ids.color_generation == srcGeneration)
        return false;
    if (!ensureColorSurface(ctx, ids.color_generation, width, height))
        return false;
    auto dstIt = m_surfaceCache.find(ids.color_generation);
    dstIt->second.guestAddr = dstGuest;
    dstIt->second.guestPitch = destination.color_pitch;
    dstIt->second.guestFormat = destination.color_format;
    dstIt->second.guestLayout = destination.layout;

    GeomDraw draw = {};
    draw.blitSrcGeneration = srcGeneration;
    draw.blitDstGeneration = ids.color_generation;
    draw.targetGuest = dstResource;
    draw.targetWidth = destination.width;
    draw.targetHeight = destination.height;
    draw.targetColorGeneration = ids.color_generation;
    draw.recordsColorWrite = 1;
    m_rec.draws.push_back(draw);
    m_latestSurfaceGeneration[dstResource] = ids.color_generation;
    m_latestSurfaceGeneration[dstGuest] = ids.color_generation;
    m_guestLatestSurfaceGeneration[dstResource] = ids.color_generation;
    m_guestLatestSurfaceGeneration[dstGuest] = ids.color_generation;
    m_guestSurfaceAddress[ids.color_generation] = dstGuest;
    return true;
}

bool PlumeDraw::setRenderTarget(const XgpuSurfaceBinding &binding)
{
    if (!binding.color_resource || binding.color_resource > UINT32_MAX ||
        !binding.width || !binding.height)
        return false;

    const PlumeSurfaceBindingIds ids = m_surfaceBindingTracker.bind(binding);
    SurfaceBindingCommand command;
    command.binding = binding;
    command.ids = ids;
    m_rec.surfaceBindings.push_back(command);

    m_currentTarget = static_cast<uint32_t>(binding.color_resource);
    m_currentTargetWidth = binding.width;
    m_currentTargetHeight = binding.height;
    m_currentColorGeneration = ids.color_generation;
    m_currentZetaGeneration = ids.zeta_generation;
    m_currentFramebufferGeneration = ids.framebuffer_generation;
    m_currentZetaFormat = binding.zeta_format;
    m_currentZetaFloat = binding.zeta_float;
    m_guestLatestSurfaceGeneration[m_currentTarget] = ids.color_generation;
    m_guestSurfaceAddress[ids.color_generation] = m_currentTarget;
    if (ids.zeta_generation) {
        if (binding.zeta_resource && binding.zeta_resource <= UINT32_MAX)
            m_guestLatestZetaGeneration[
                static_cast<uint32_t>(binding.zeta_resource)] =
                    ids.zeta_generation;
        if (binding.zeta_guest_address)
            m_guestLatestZetaGeneration[binding.zeta_guest_address] =
                ids.zeta_generation;
    }
    return true;
}

bool PlumeDraw::applySurfaceBinding(
    PlumeContext &ctx,
    const SurfaceBindingCommand &command)
{
    const XgpuSurfaceBinding &binding = command.binding;
    const PlumeSurfaceBindingIds &ids = command.ids;

    /* Allocate the whole buffer the clip rectangle sits inside, not just the
     * rectangle: a texture may alias this address and read the full stride.
     * The viewport/NDC path below deliberately keeps using binding.width /
     * binding.height -- substituting the allocation there would rescale every
     * vertex against the wrong half-width. The zeta allocation has its own
     * extent because Xbox titles can bind one full-size depth surface with
     * smaller offscreen color targets. */
    uint32_t imageWidth = binding.image_width ? binding.image_width
                                              : binding.width;
    uint32_t imageHeight = binding.image_height ? binding.image_height
                                                : binding.height;
    uint32_t zetaWidth = binding.zeta_width ? binding.zeta_width : imageWidth;
    uint32_t zetaHeight =
        binding.zeta_height ? binding.zeta_height : imageHeight;
    if (!ensureColorSurface(ctx, ids.color_generation, imageWidth,
                            imageHeight)) {
        fprintf(stderr, "[PLUME-RT] FAIL color guest=%08X\n",
                (uint32_t)binding.color_resource);
        return false;
    }
    auto colorIt = m_surfaceCache.find(ids.color_generation);
    colorIt->second.guestAddr =
        static_cast<uint32_t>(binding.color_resource);
    colorIt->second.guestPitch = binding.color_pitch;
    colorIt->second.guestFormat = binding.color_format;
    colorIt->second.guestLayout = binding.layout;

    auto zetaIt = m_zetaCache.end();
    if (ids.zeta_generation) {
        zetaIt = m_zetaCache.find(ids.zeta_generation);
        if (zetaIt == m_zetaCache.end()) {
            PlumeZetaSurface zeta;
            zeta.width = zetaWidth;
            zeta.height = zetaHeight;
            if (!plumeScaledExtent(zetaWidth, zetaHeight,
                                   m_internalResolutionScale,
                                   &zeta.physicalWidth,
                                   &zeta.physicalHeight))
                return false;
            zeta.format = plume_depth_format_from_xgpu(binding.zeta_format,
                                                        binding.zeta_float);
            if (zeta.format == RenderFormat::UNKNOWN) {
                fprintf(stderr, "[PLUME-RT] FAIL zeta guest=%08X zfmt=%u\n",
                        (uint32_t)binding.color_resource,
                        binding.zeta_format);
                return false;
            }
            zeta.texture = ctx.device()->createTexture(
                RenderTextureDesc::DepthTarget(zeta.physicalWidth,
                                               zeta.physicalHeight,
                                               zeta.format));
            if (!zeta.texture)
                return false;
            zeta.layout = RenderTextureLayout::UNKNOWN;
            zetaIt = m_zetaCache.emplace(ids.zeta_generation,
                                         std::move(zeta)).first;
        }
        /* Guest identity, refreshed every bind: a texture aliasing this
         * address needs the depth resolved back to guest memory. */
        zetaIt->second.guestAddr = binding.zeta_guest_address;
        if (!zetaIt->second.guestAddr &&
            binding.zeta_resource <= UINT32_MAX)
            zetaIt->second.guestAddr =
                static_cast<uint32_t>(binding.zeta_resource);
        zetaIt->second.guestPitch = binding.zeta_pitch;
        if (binding.zeta_resource &&
            binding.zeta_resource <= UINT32_MAX)
            m_latestZetaGeneration[(uint32_t)binding.zeta_resource] =
                ids.zeta_generation;
        if (binding.zeta_guest_address)
            m_latestZetaGeneration[binding.zeta_guest_address] =
                ids.zeta_generation;
    }

    auto framebufferIt = m_framebufferCache.find(ids.framebuffer_generation);
    if (framebufferIt == m_framebufferCache.end()) {
        PlumeFramebuffer framebuffer;
        const RenderTexture *color = colorIt->second.texture.get();
        const RenderTexture *depth = zetaIt != m_zetaCache.end()
            ? zetaIt->second.texture.get() : nullptr;
        framebuffer.framebuffer = ctx.device()->createFramebuffer(
            RenderFramebufferDesc(&color, 1, depth));
        if (!framebuffer.framebuffer)
            return false;
        framebuffer.colorGeneration = ids.color_generation;
        framebuffer.zetaGeneration = ids.zeta_generation;
        m_framebufferCache.emplace(ids.framebuffer_generation,
                                   std::move(framebuffer));
    }

    m_latestSurfaceGeneration[
        static_cast<uint32_t>(binding.color_resource)] = ids.color_generation;
    return true;
}

void PlumeDraw::consumeSurfaceBindings(PlumeContext &ctx,
                                       FrameRecording &recording)
{
    for (const SurfaceBindingCommand &command : recording.surfaceBindings)
        (void)applySurfaceBinding(ctx, command);
    recording.surfaceBindings.clear();
}

void PlumeDraw::clearTarget(float r, float g, float b, float a,
                            uint32_t colorWriteMask,
                            const XgpuRect *rect)
{
    colorWriteMask &= 0xFu;
    if (!colorWriteMask)
        return;
    if (colorWriteMask != 0xFu) {
        const uint32_t red = uint32_t(r * 255.0f + 0.5f) & 0xFFu;
        const uint32_t green = uint32_t(g * 255.0f + 0.5f) & 0xFFu;
        const uint32_t blue = uint32_t(b * 255.0f + 0.5f) & 0xFFu;
        const uint32_t alpha = uint32_t(a * 255.0f + 0.5f) & 0xFFu;
        const uint32_t diffuse =
            (alpha << 24) | (red << 16) | (green << 8) | blue;
        PlumeClearVertex vertices[6];
        plume_make_clear_quad(rect, m_currentTargetWidth,
                              m_currentTargetHeight, diffuse, vertices);
        const uint32_t byteLen = sizeof(vertices);
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(vertices);
        uint32_t off = 0;
        if (!plumeAppendVertexBytes(
                m_rec.frameVerts, bytes, byteLen, &off)) {
            xgpu_plume_f2_log(
                "skip reason=vertex-stream-overflow kind=masked-clear "
                "current=%zu add=%u",
                m_rec.frameVerts.size(), byteLen);
            return;
        }

        GeomDraw draw = {};
        draw.offset = off;
        draw.byteLen = byteLen;
        draw.vertexCount = 6;
        draw.stride = sizeof(PlumeClearVertex);
        draw.topology = (uint8_t)RenderPrimitiveTopology::TRIANGLE_LIST;
        draw.hasDiffuse = 1;
        draw.renderState = plume_draw_default_state();
        draw.renderState.blend_enable = 0;
        draw.renderState.depth_enable = 0;
        draw.renderState.depth_write = 0;
        draw.renderState.stencil_enable = 0;
        draw.renderState.color_write_mask = colorWriteMask;
        draw.targetGuest = m_currentTarget;
        draw.targetWidth = m_currentTargetWidth;
        draw.targetHeight = m_currentTargetHeight;
        draw.targetColorGeneration = m_currentColorGeneration;
        draw.targetZetaGeneration = m_currentZetaGeneration;
        draw.targetFramebufferGeneration = m_currentFramebufferGeneration;
        draw.recordsColorWrite = 1;
        m_rec.draws.push_back(draw);
        return;
    }
    GeomDraw draw = {};
    draw.targetGuest = m_currentTarget;
    draw.targetWidth = m_currentTargetWidth;
    draw.targetHeight = m_currentTargetHeight;
    draw.targetColorGeneration = m_currentColorGeneration;
    draw.targetZetaGeneration = m_currentZetaGeneration;
    draw.targetFramebufferGeneration = m_currentFramebufferGeneration;
    draw.clear = 1;
    draw.clearColor[0] = r;
    draw.clearColor[1] = g;
    draw.clearColor[2] = b;
    draw.clearColor[3] = a;
    if (rect) {
        draw.hasClearRect = 1;
        draw.clearRect = *rect;
    }
    draw.recordsColorWrite = 1;
    m_rec.draws.push_back(draw);
}

void PlumeDraw::clearDepthStencil(bool clearDepth, bool clearStencil,
                                  float depth, uint8_t stencil,
                                  const XgpuRect *rect)
{
    GeomDraw draw = {};
    draw.targetGuest = m_currentTarget;
    draw.targetWidth = m_currentTargetWidth;
    draw.targetHeight = m_currentTargetHeight;
    draw.targetColorGeneration = m_currentColorGeneration;
    draw.targetZetaGeneration = m_currentZetaGeneration;
    draw.targetFramebufferGeneration = m_currentFramebufferGeneration;
    draw.clearDepth = clearDepth ? 1 : 0;
    draw.clearStencil = clearStencil ? 1 : 0;
    draw.depthClear = depth;
    draw.stencilClear = stencil;
    if (rect) {
        draw.hasClearRect = 1;
        draw.clearRect = *rect;
    }
    draw.recordsZetaWrite = clearDepth ? 1u : 0u;
    m_rec.draws.push_back(draw);
}

bool PlumeDraw::setSurfaceTexture(uint32_t stage, uint32_t guest,
                                  uint32_t unnormalizedCoords)
{
    if (stage >= 4)
        return false;
    m_curTextureStage[stage] = {};
    m_curSurfaceStage[stage] = 0;
    m_curSurfaceUnnormalized[stage] = 0;
    if (!guest)
        return true;
    auto latest = m_guestLatestSurfaceGeneration.find(guest);
    if (latest == m_guestLatestSurfaceGeneration.end())
        return false;
    m_curSurfaceStage[stage] = latest->second;
    m_curSurfaceUnnormalized[stage] = unnormalizedCoords ? 1u : 0u;
    return true;
}

void PlumeDraw::setPresentSurface(uint32_t guest)
{
    m_presentTarget = guest;
}

uint64_t PlumeDraw::normalizeSampledSurfaceGeneration(
    uint64_t sampledGeneration, uint64_t targetGeneration) const
{
    auto sampled = m_guestSurfaceAddress.find(sampledGeneration);
    auto target = m_guestSurfaceAddress.find(targetGeneration);
    if (sampled == m_guestSurfaceAddress.end() ||
        target == m_guestSurfaceAddress.end())
        return sampledGeneration;

    const uint64_t normalized = plumeNormalizeSampledSurfaceGeneration(
        sampledGeneration, sampled->second,
        targetGeneration, target->second);
    if (normalized != sampledGeneration && xgpu_plume_f2_active()) {
        xgpu_plume_f2_log(
            "surface-alias sample=%llu target=%llu guest=%08X -> snapshot",
            static_cast<unsigned long long>(sampledGeneration),
            static_cast<unsigned long long>(targetGeneration),
            target->second);
    }
    return normalized;
}

PlumeDraw::PlumeTex *PlumeDraw::resolveTextureBinding(
    const RecordedTextureBinding &binding)
{
    auto *entry = m_textures.resolve(binding);
    return entry ? &entry->resource : nullptr;
}

const PlumeDraw::PlumeTex *PlumeDraw::resolveTextureBinding(
    const RecordedTextureBinding &binding) const
{
    const auto *entry = m_textures.resolve(binding);
    return entry ? &entry->resource : nullptr;
}

template <class Draw>
::plume::RenderDescriptorSet *PlumeDraw::createProgDrawDescriptorSet(
    PlumeContext &ctx, const Draw &draw,
    const ::plume::RenderBuffer *constantBuffer,
    uint64_t constantOffset,
    const ::plume::RenderBuffer *vertexConstantBuffer,
    uint64_t vertexConstantBufferSize)
{
    if (!constantBuffer)
        return nullptr;

    std::array<uint64_t, 10> key = {};
    RenderTexture *textures[4] = {};
    RenderTextureView *views[4] = {};
    RenderSampler *samplers[4] = {};
    uint8_t cubeTextureMask = 0;
    const auto psIt = m_psReg.find(draw.psHandle);
    if (psIt != m_psReg.end())
        cubeTextureMask = psIt->second.cubeTextureMask;
    for (int s = 0; s < 4; s++) {
        if (draw.surfaceStage[s]) {
            auto it = m_surfaceCache.find(draw.surfaceStage[s]);
            if (it != m_surfaceCache.end()) {
                const bool self =
                    draw.surfaceStage[s] == draw.targetColorGeneration;
                textures[s] = self ? it->second.snapshot.get()
                                   : it->second.texture.get();
                views[s] = self ? it->second.snapshotView.get()
                                : it->second.view.get();
            }
        } else {
            PlumeTex *texture = resolveTextureBinding(draw.stageTexture[s]);
            if (texture) {
                textures[s] = texture->tex.get();
                views[s] = texture->view.get();
            }
        }
        samplers[s] = samplerForBinding(
            ctx, draw.stageSamplerState[s],
            draw.stageSamplerStateValid[s] != 0);
        if (!samplers[s])
            samplers[s] = m_progSampler.get();
        key[s] = textures[s]
            ? (uint64_t)(uintptr_t)textures[s]
            : ((cubeTextureMask & (1u << s)) ? 1u : 0u);
        key[4 + s] = (uint64_t)(uintptr_t)samplers[s];
    }
    key[8] = constantOffset;
    key[9] = vertexConstantBuffer ? UINT64_MAX : 0;

    auto it = m_progDescCache.find(key);
    if (it != m_progDescCache.end())
        return it->second.get();

    const ProgramBindingLayout bindingLayout = vertexConstantBuffer
        ? makeProgramIndexedBindingLayout()
        : makeProgramBindingLayout();
    RenderDescriptorSetDesc dsd(bindingLayout.ranges.data(),
                                bindingLayout.rangeCount);
    std::unique_ptr<RenderDescriptorSet> set = ctx.device()->createDescriptorSet(dsd);
    if (!set)
        return nullptr;
    for (int s = 0; s < 4; s++) {                    /* flat indices: tex 0-3 */
        if (textures[s])
            set->setTexture((uint32_t)s, textures[s],
                            RenderTextureLayout::SHADER_READ, views[s]);
        else if (cubeTextureMask & (1u << s))
            set->setTexture((uint32_t)s, m_whiteCubeTex.get(),
                            RenderTextureLayout::SHADER_READ,
                            m_whiteCubeView.get());
        else
            set->setTexture((uint32_t)s, m_whiteTex.get(),
                            RenderTextureLayout::SHADER_READ,
                            m_whiteView.get());
    }
    for (int s = 0; s < 4; s++) {                     /* flat indices: samp 4-7 */
        set->setSampler((uint32_t)(4 + s), samplers[s]);
    }
    set->setBuffer(8, constantBuffer, kProgramConstantStride,
                   nullptr, nullptr, constantOffset);
    if (vertexConstantBuffer && vertexConstantBufferSize) {
        const RenderBufferStructuredView float4View(4u * sizeof(float));
        set->setBuffer(9, vertexConstantBuffer, vertexConstantBufferSize,
                       &float4View);
    }

    RenderDescriptorSet *raw = set.get();
    m_progDescCache.emplace(key, std::move(set));
    return raw;
}

/* Lazily create + cache the Plume RenderShader for a translated pixel shader. */
::plume::RenderShader *PlumeDraw::progPixelShader(PlumeContext &ctx, uint32_t handle)
{
    auto it = m_psReg.find(handle);
    if (it == m_psReg.end() || !it->second.ok)
        return nullptr;
    PlumePixelShader &ps = it->second;
    if (!ps.shader) {
        if (ps.bytecode.empty()) {
            if (!ps.compileRetry.shouldAttempt())
                return nullptr;
            if (!ps.compileFuture.valid()) {
                ps.compileFuture = queueShaderCompile(
                    m_recordShaderTarget, ps.hlsl, "ps_6_0");
            }
            if (!shaderCompileReady(ps.compileFuture))
                return nullptr;
            ShaderCompileResult compiled = ps.compileFuture.get();
            if (!compiled.ok) {
                /*
                 * Translation validity and a runtime compiler failure are
                 * different states. Cache the failure for this exact source
                 * revision so a deterministic error is not recompiled for
                 * every draw. A live-source replacement advances the revision
                 * and can recover normally.
                 */
                ps.compileRetry.noteFailure();
                ps.diagnostics = compiled.diagnostics;
                std::fprintf(stderr, "[PLUME] shader compile failed: %s\n",
                             ps.diagnostics.c_str());
                return nullptr;
            }
            ps.compileRetry.noteSuccess();
            ps.diagnostics.clear();
            ps.target = compiled.target;
            ps.entryPoint = std::move(compiled.entryPoint);
            ps.bytecode = std::move(compiled.bytecode);
        }
        const RenderShaderFormat format =
            renderShaderFormatForTarget(ps.target);
        ps.shader = ctx.device()->createShader(
            ps.bytecode.data(), ps.bytecode.size(), ps.entryPoint.c_str(),
            format);
    }
    return ps.shader.get();
}

void PlumeDraw::pollPixelShaderOverrides(PlumeContext &ctx)
{
    if (!live_shader_override_dir())
        return;
    const uint64_t now = xrecomp_host_monotonic_ms();
    if (m_liveShaderPollMs && now - m_liveShaderPollMs < 100u)
        return;
    m_liveShaderPollMs = now;

    for (auto &entry : m_psReg) {
        const uint32_t handle = entry.first;
        PlumePixelShader &ps = entry.second;
        if (ps.livePath.empty())
            continue;
        const int64_t stamp = live_shader_write_stamp(ps.livePath);
        if (!stamp || stamp == ps.liveWriteStamp)
            continue;
        /* Remember a failed edit too. A subsequent save changes the timestamp
         * and retries, while the last successfully-created GPU shader remains
         * active in the meantime. */
        ps.liveWriteStamp = stamp;
        std::string source;
        if (!read_live_shader(ps.livePath, source)) {
            std::fprintf(stderr,
                         "[PLUME-LIVE] pixel shader %08X edit is empty or "
                         "unreadable; keeping the last valid shader\n",
                         handle);
            continue;
        }
        ShaderCompileResult compiled =
            compileForContext(ctx, source.c_str(), "main", "ps_6_0");
        if (!compiled.ok) {
            std::fprintf(stderr,
                         "[PLUME-LIVE] pixel shader %08X rejected; keeping "
                         "the last valid shader\n",
                         handle);
            continue;
        }
        std::unique_ptr<RenderShader> replacement =
            ctx.device()->createShader(
                compiled.bytecode.data(), compiled.bytecode.size(),
                compiled.entryPoint.c_str(),
                renderShaderFormatForTarget(compiled.target));
        if (!replacement) {
            std::fprintf(stderr,
                         "[PLUME-LIVE] pixel shader %08X backend creation "
                         "failed; keeping the last valid shader\n",
                         handle);
            continue;
        }

        if (ps.shader)
            m_liveRetiredShaders.push_back(std::move(ps.shader));
        for (auto &pipeline : m_progPsos)
            m_liveRetiredPipelines.push_back(std::move(pipeline.second));
        m_progPsos.clear();
        for (auto &pipeline : m_progIdxPsos)
            m_liveRetiredPipelines.push_back(std::move(pipeline.second));
        m_progIdxPsos.clear();

        ps.hlsl = std::move(source);
        ps.compileRetry.noteSourceChange();
        ps.compileRetry.noteSuccess();
        ps.compileFuture = {};
        ps.bytecode = std::move(compiled.bytecode);
        ps.entryPoint = std::move(compiled.entryPoint);
        ps.target = compiled.target;
        ps.shader = std::move(replacement);
        ps.ok = true;
        ps.combinerCB =
            ps.hlsl.find("cbuffer CombinerConsts") != std::string::npos ||
            ps.hlsl.find("cbuffer CombinerCB") != std::string::npos;
        std::fprintf(stderr,
                     "[PLUME-LIVE] reloaded pixel shader %08X from %s\n",
                     handle, ps.livePath.c_str());
    }
}

::plume::RenderShader *PlumeDraw::progVertexShader(PlumeContext &ctx,
                                                   uint32_t handle)
{
    auto it = m_vsReg.find(handle);
    if (it == m_vsReg.end() || !it->second.ok)
        return nullptr;
    PlumeVertexShader &vs = it->second;
    if (!vs.shader) {
        if (vs.bytecode.empty()) {
            if (!vs.compileFuture.valid()) {
                vs.compileFuture = queueShaderCompile(
                    m_recordShaderTarget, vs.hlsl, "vs_6_0");
            }
            if (!shaderCompileReady(vs.compileFuture))
                return nullptr;
            ShaderCompileResult compiled = vs.compileFuture.get();
            if (!compiled.ok) {
                vs.diagnostics = compiled.diagnostics;
                vs.ok = false;
                std::fprintf(stderr, "[PLUME] shader compile failed: %s\n",
                             vs.diagnostics.c_str());
                return nullptr;
            }
            vs.diagnostics.clear();
            vs.target = compiled.target;
            vs.entryPoint = std::move(compiled.entryPoint);
            vs.bytecode = std::move(compiled.bytecode);
        }
        const RenderShaderFormat format =
            renderShaderFormatForTarget(vs.target);
        vs.shader = ctx.device()->createShader(
            vs.bytecode.data(), vs.bytecode.size(), vs.entryPoint.c_str(),
            format);
    }
    return vs.shader.get();
}

/* Programmable PSO keyed on translated shaders, layout, and render state. */
::plume::RenderPipeline *PlumeDraw::progPso(
    PlumeContext &ctx, uint32_t psHandle, uint32_t vsHandle, uint32_t stride,
    uint8_t topology, uint8_t hasDiffuse, uint8_t hasSpecular,
    uint8_t hasUV, uint8_t texCount, uint8_t uvOffset, uint32_t fvf,
    const XgpuPlumeRenderState &renderState, bool hasDepthAttachment,
    ProgPsoFailure *failure)
{
    if (failure)
        *failure = {};
    PlumeFvfTexcoordLayout fixedTexcoords;
    const bool fixedTexcoordsValid =
        vsHandle || plumeDecodeFvfTexcoordLayout(
            fvf, uvOffset, stride, fixedTexcoords);
    const uint8_t uvKind =
        (!vsHandle && hasUV && fixedTexcoordsValid) ? fixedTexcoords.count : 0u;
    uint32_t baseKey = (psHandle << 19) ^ (vsHandle * 0x45d9f3bu) ^
                       (stride << 9) ^ ((uint32_t)uvOffset << 4) ^
                       ((uint32_t)topology << 3) ^ ((uint32_t)hasDiffuse << 2) ^
                       ((uint32_t)hasSpecular << 1) ^ hasUV ^
                       ((uint32_t)uvKind << 8);
    uint64_t key = plume_render_state_key(renderState) ^
                   (uint64_t(baseKey) * 0x9E3779B185EBCA87ull) ^
                   (uint64_t(vsHandle ? 0u : (fvf >> 16)) *
                     0xD6E8FEB86659FD93ull);
    key = (key * 1099511628211ull) ^ (hasDepthAttachment ? 1ull : 0ull);
    auto it = m_progPsos.find(key);
    if (it != m_progPsos.end()) {
        if (!it->second && failure)
            failure->reason = "cached-create";
        return it->second.get();
    }

    RenderShader *ps = this->progPixelShader(ctx, psHandle);
    if (!ps) {
        if (failure)
            failure->reason = "pixel-shader";
        return nullptr;
    }

    RenderInputSlot slot(0, stride);
    RenderInputElement elems[16];
    uint32_t ne = 0;
    uint32_t slotIndex = 0;
    RenderShader *vertexShader = nullptr;
    if (vsHandle) {
        auto vsIt = m_vsReg.find(vsHandle);
        if (vsIt == m_vsReg.end()) {
            if (failure)
                failure->reason = "vertex-registry";
            return nullptr;
        }
        if (vsIt->second.hasDeclaration) {
            const uint16_t latchInputs =
                (uint16_t)(vsIt->second.inputsRead &
                           (uint16_t)~vsIt->second.attributesPresent);
            uint32_t latchCount = 0;
            for (uint32_t input = 0; input < 16; ++input)
                if (latchInputs & (uint16_t)(1u << input))
                    ++latchCount;
            const uint32_t latchBytes =
                latchCount * 4u * sizeof(float);
            if (latchBytes > stride) {
                if (failure)
                    failure->reason = "latch-stride";
                return nullptr;
            }
            const uint32_t sourceStride = stride - latchBytes;
            uint32_t latchOffset = sourceStride;
            for (uint32_t input = 0; input < 16; ++input) {
                if (!(vsIt->second.inputsRead & (1u << input)))
                    continue;
                if (!(vsIt->second.attributesPresent & (1u << input))) {
                    elems[ne++] = RenderInputElement(
                        "ATTR", input, slotIndex++,
                        RenderFormat::R32G32B32A32_FLOAT, 0, latchOffset);
                    latchOffset += 4u * sizeof(float);
                    continue;
                }
                if (vsIt->second.stream[input] != 0) {
                    if (failure) {
                        failure->reason = "nonzero-stream";
                        failure->attr = input;
                        failure->stream = vsIt->second.stream[input];
                        failure->format = vsIt->second.format[input];
                        failure->offset = vsIt->second.offset[input];
                    }
                    return nullptr;
                }
                RenderFormat format =
                    nv2a_attr_render_format(vsIt->second.format[input]);
                uint32_t size =
                    nv2a_attr_host_size(vsIt->second.format[input]);
                uint32_t offset = vsIt->second.offset[input];
                if (format == RenderFormat::UNKNOWN || size == 0) {
                    if (failure) {
                        failure->reason = "unsupported-format";
                        failure->attr = input;
                        failure->format = vsIt->second.format[input];
                        failure->offset = offset;
                        failure->size = size;
                    }
                    return nullptr;
                }
                if (offset > sourceStride ||
                    size > sourceStride - offset) {
                    if (failure) {
                        failure->reason = "attr-range";
                        failure->attr = input;
                        failure->format = vsIt->second.format[input];
                        failure->offset = offset;
                        failure->size = size;
                    }
                    return nullptr;
                }
                elems[ne++] = RenderInputElement(
                    "ATTR", input, slotIndex++, format, 0, offset);
            }
        } else {
            /* Compatibility path for translated programs registered without
             * an Xbox declaration (for example direct NV2A experiments). */
            uint32_t offset = 0;
            for (uint32_t input = 0; input < 16; ++input) {
                if (!(vsIt->second.inputsRead & (1u << input)))
                    continue;
                RenderFormat format;
                uint32_t size;
                switch (input) {
                case 0: case 2:
                    format = RenderFormat::R32G32B32_FLOAT; size = 12; break;
                case 3: case 4: case 7:
                    format = RenderFormat::R8G8B8A8_UNORM; size = 4; break;
                case 5: case 6:
                    format = RenderFormat::R32_FLOAT; size = 4; break;
                case 8: case 9: case 10: case 11:
                    format = RenderFormat::R32G32_FLOAT; size = 8; break;
                default:
                    format = RenderFormat::R32G32B32A32_FLOAT; size = 16; break;
                }
                if (offset + size > stride)
                    return nullptr;
                elems[ne++] = RenderInputElement(
                    "ATTR", input, slotIndex++, format, 0, offset);
                offset += size;
            }
        }
        vertexShader = progVertexShader(ctx, vsHandle);
    } else {
        elems[ne++] = RenderInputElement(
            "POSITION", 0, slotIndex++, RenderFormat::R32G32B32A32_FLOAT, 0, 0);
        if (hasDiffuse)
            elems[ne++] = RenderInputElement(
                "COLOR", 0, slotIndex++, RenderFormat::R8G8B8A8_UNORM, 0, 16);
        if (hasSpecular)
            elems[ne++] = RenderInputElement(
                "COLOR", 1, slotIndex++, RenderFormat::R8G8B8A8_UNORM, 0,
                16 + hasDiffuse * 4);
        if (hasUV) {
            if (!fixedTexcoordsValid || fixedTexcoords.count != texCount) {
                if (failure)
                    failure->reason = "fvf-texcoord-layout";
                return nullptr;
            }
            for (uint32_t uv = 0; uv < fixedTexcoords.count; ++uv) {
                RenderFormat format = RenderFormat::UNKNOWN;
                switch (fixedTexcoords.components[uv]) {
                case 1: format = RenderFormat::R32_FLOAT; break;
                case 2: format = RenderFormat::R32G32_FLOAT; break;
                case 3: format = RenderFormat::R32G32B32_FLOAT; break;
                case 4: format = RenderFormat::R32G32B32A32_FLOAT; break;
                }
                if (format == RenderFormat::UNKNOWN) {
                    if (failure)
                        failure->reason = "fvf-texcoord-format";
                    return nullptr;
                }
                elems[ne++] = RenderInputElement(
                    "TEXCOORD", uv, slotIndex++, format, 0,
                    fixedTexcoords.offsets[uv]);
            }
        }
        if (uvKind > 4u) {
            if (failure)
                failure->reason = "fvf-texcoord-count";
            return nullptr;
        }
        const uint32_t vsIndex =
            ((uint32_t)hasDiffuse * 2u + (uint32_t)hasSpecular) * 5u +
            uvKind;
        vertexShader = m_progVS[vsIndex].get();
    }
    if (!vertexShader || !ne) {
        if (failure)
            failure->reason = !vertexShader ? "vertex-shader" : "no-elements";
        return nullptr;
    }

    RenderGraphicsPipelineDesc pd;
    /* Translated Xbox vertex shaders read their 192 float4 constants from
     * t4.  Fixed-function host vertex shaders do not, so keep their smaller
     * layout but use the shared indexed-program layout whenever a translated
     * vertex shader is active. */
    pd.pipelineLayout = vsHandle ? m_progIdxLayout.get()
                                 : m_progLayout.get();
    pd.vertexShader = vertexShader;
    pd.pixelShader = ps;
    pd.inputSlots = &slot;
    pd.inputSlotsCount = 1;
    pd.inputElements = elems;
    pd.inputElementsCount = ne;
    pd.renderTargetFormat[0] = RenderFormat::B8G8R8A8_UNORM;
    pd.renderTargetCount = 1;
    pd.primitiveTopology = (RenderPrimitiveTopology)topology;
    plume_draw_apply_state(pd, renderState, hasDepthAttachment);

    XRECOMP_TRACY_ZONE_SCOPED("Plume Create Graphics Pipeline");
    uint64_t perf_pipeline_t0 = xgpu_plume_perf_begin();
    std::unique_ptr<RenderPipeline> pso =
        ctx.device()->createGraphicsPipeline(pd);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_PIPELINE, perf_pipeline_t0);
    if (!pso) {
        if (failure)
            failure->reason = "create-pipeline";
        return nullptr;
    }
    RenderPipeline *raw = pso.get();
    m_progPsos.emplace(key, std::move(pso));
    return raw;
}

std::array<float, 16> PlumeDraw::snapshotProgramTextureScales() const
{
    std::array<float, 16> scales = {};
    /*
     * D3D12/Vulkan samplers always consume normalized coordinates. Xbox
     * pitch-linear textures instead consume texel-space coordinates. Keep
     * that distinction with the texture binding and snapshot the conversion
     * with each deferred draw so later SetTexture calls cannot alter it.
     */
    for (uint32_t stage = 0; stage < 4; ++stage) {
        float *scale = scales.data() + stage * 4u;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;
        uint32_t unnormalized = 0;
        const RecordedTextureBinding &texture = m_curTextureStage[stage];
        if (texture.valid()) {
            width = texture.width;
            height = texture.height;
            depth = texture.depth;
            unnormalized = texture.unnormalizedCoords;
        } else if (m_curSurfaceStage[stage]) {
            auto surface = m_surfaceCache.find(m_curSurfaceStage[stage]);
            if (surface != m_surfaceCache.end()) {
                width = surface->second.width;
                height = surface->second.height;
                unnormalized = m_curSurfaceUnnormalized[stage];
            }
        }
        const std::array<float, 4> textureScale =
            plumeTextureCoordinateScale(
                width, height, depth, unnormalized);
        std::copy(textureScale.begin(), textureScale.end(), scale);
    }
    return scales;
}

std::array<float, kProgramConstantFloatCount>
PlumeDraw::snapshotProgramConstants(
    bool combinerCB, const std::array<float, 16> &textureScales,
    const XgpuPlumeRenderState &renderState) const
{
    std::array<float, kProgramConstantFloatCount> constants = {};
    const uint32_t control[4] = {
        renderState.stipple_enable,
        std::max(m_internalResolutionScale, 1u),
        0u,
        0u,
    };
    if (combinerCB)
        std::memcpy(constants.data(), m_combinerConst,
                    sizeof(m_combinerConst));
    else
        std::memcpy(constants.data(), m_psConst, sizeof(m_psConst));
    std::memcpy(constants.data() + 21u * 4u, textureScales.data(),
                textureScales.size() * sizeof(float));
    std::memcpy(constants.data() + 25u * 4u, renderState.stipple_pattern,
                sizeof(renderState.stipple_pattern));
    std::memcpy(constants.data() + 33u * 4u, control, sizeof(control));
    return constants;
}

uint32_t PlumeDraw::internProgramConstants(
    bool combinerCB, const XgpuPlumeRenderState &renderState)
{
    const std::array<float, 16> textureScales =
        snapshotProgramTextureScales();
    const uint64_t sourceVersion = combinerCB
        ? m_combinerConstVersion : m_psConstVersion;
    const uint64_t cacheVersion =
        (sourceVersion << 1u) | (combinerCB ? 1u : 0u);
    uint32_t index = UINT32_MAX;
    if (!renderState.stipple_enable && m_rec.frameProgConstCache.lookup(
            cacheVersion, textureScales, index))
        return index;
    const std::array<float, kProgramConstantFloatCount> constants =
        snapshotProgramConstants(combinerCB, textureScales, renderState);
    index = internFrameConstants(
        m_rec.frameProgConsts, m_rec.frameProgConstBuckets, constants);
    if (!renderState.stipple_enable)
        m_rec.frameProgConstCache.remember(
            cacheVersion, textureScales, index);
    return index;
}

void PlumeDraw::recordDraw(PlumeContext &ctx, uint32_t primType, uint32_t primCount,
                           const void *verts, uint32_t stride, uint32_t fvf,
                           const XgpuPlumeRenderState *renderState)
{
    (void)ctx;
    if (!verts || !stride || primCount == 0)
        return;
    XRECOMP_TRACY_ZONE_SCOPED("Plume Record CPU Draw");
    XRECOMP_CPU_RECORDER_ZONE_SCOPED("Plume Record Draw");
    const uint32_t vsH = m_vsReg.find(m_activeVS) != m_vsReg.end()
        ? m_activeVS : 0;
    /* Fixed-function records always begin with XYZRHW.  Programmable records
     * are governed by their Xbox declaration instead: a valid declaration
     * can be as small as one packed CMP dword (4 bytes).  progPso validates
     * every read declaration element against the recorded stride before it
     * creates an input layout, so imposing a float3-sized minimum here only
     * drops valid packed streams before that validation can run. */
    if (!vsH && stride < 16u)
        return;
    uint32_t vc = 0;
    uint8_t topo = 0;
    switch ((D3DPRIMITIVETYPE)primType) {
    case D3DPT_POINTLIST:
        vc = primCount;
        topo = (uint8_t)RenderPrimitiveTopology::POINT_LIST;
        break;
    case D3DPT_LINELIST:
        vc = primCount * 2u;
        topo = (uint8_t)RenderPrimitiveTopology::LINE_LIST;
        break;
    case D3DPT_LINESTRIP:
        vc = primCount + 1u;
        topo = (uint8_t)RenderPrimitiveTopology::LINE_STRIP;
        break;
    case D3DPT_TRIANGLELIST:
        vc = primCount * 3u;
        topo = (uint8_t)RenderPrimitiveTopology::TRIANGLE_LIST;
        break;
    case D3DPT_TRIANGLESTRIP:
        vc = primCount + 2u;
        topo = (uint8_t)RenderPrimitiveTopology::TRIANGLE_STRIP;
        break;
    case D3DPT_TRIANGLEFAN:
        vc = primCount + 2u;
        topo = (uint8_t)RenderPrimitiveTopology::TRIANGLE_FAN;
        break;
    default: return;
    }

    const uint8_t hasDiffuse = vsH ? 0u : ((fvf & 0x40u) ? 1u : 0u);
    const uint8_t hasSpecular = vsH ? 0u : ((fvf & 0x80u) ? 1u : 0u);
    /* UV sits after XYZRHW + optional diffuse(0x40) + optional specular(0x80). */
    const uint8_t uvOffset = (uint8_t)(16 + ((fvf & 0x40u) ? 4 : 0) + ((fvf & 0x80u) ? 4 : 0));
    PlumeFvfTexcoordLayout fixedTexcoords;
    const bool fixedTexcoordsValid =
        vsH || plumeDecodeFvfTexcoordLayout(
            fvf, uvOffset, stride, fixedTexcoords);
    const uint8_t texCount =
        vsH || !fixedTexcoordsValid ? 0u : fixedTexcoords.count;
    const uint8_t *src = (const uint8_t *)verts;
    const uint8_t hasUV = texCount >= 1u ? 1u : 0u;
    std::vector<uint8_t> latchedVertices;
    if (vsH) {
        const auto vsIt = m_vsReg.find(vsH);
        const uint16_t latchInputs =
            vsIt == m_vsReg.end() || !vsIt->second.hasDeclaration
                ? 0u
                : (uint16_t)(vsIt->second.inputsRead &
                             (uint16_t)~vsIt->second.attributesPresent);
        if (latchInputs) {
            LatchedVertexArray array = {};
            if (!materializeLatchedVertexArray(
                    latchedVertices, src, vc, stride, latchInputs,
                    &m_vertexData[0][0], &array))
                return;
            src = latchedVertices.data();
            stride = array.stride;
        }
    }
    XgpuPlumeRenderState drawState = renderState
        ? *renderState : plume_draw_default_state();
    drawState.zeta_format = m_currentZetaFormat;
    drawState.zeta_float = m_currentZetaFloat;

    std::vector<uint8_t> uiCanvasVertices;
    const uint64_t sourceByteLength = static_cast<uint64_t>(vc) * stride;
    if (!vsH && drawState.position_mode != 0u
        && drawState.ui_canvas_active
        && drawState.ui_canvas_width > 0.0f
        && drawState.ui_canvas_height > 0.0f
        && m_currentTargetWidth != 0u
        && std::fabs(static_cast<float>(m_currentTargetHeight)
                     - drawState.ui_canvas_height) <= 1.0f
        && sourceByteLength <= UINT32_MAX) {
        float minimumX;
        float maximumX;
        std::memcpy(&minimumX, src, sizeof(minimumX));
        maximumX = minimumX;
        bool finitePositions = std::isfinite(minimumX);
        for (uint32_t vertex = 1; vertex < vc && finitePositions; ++vertex) {
            float x;
            std::memcpy(&x, src + static_cast<size_t>(vertex) * stride,
                        sizeof(x));
            finitePositions = std::isfinite(x);
            minimumX = std::min(minimumX, x);
            maximumX = std::max(maximumX, x);
        }
        PlumeUiCanvasHorizontalTransform transform;
        if (finitePositions && plumeComputeUiCanvasHorizontalTransform(
                minimumX, maximumX,
                drawState.ui_canvas_width,
                static_cast<float>(m_currentTargetWidth),
                static_cast<float>(drawState.viewport_x),
                static_cast<float>(drawState.viewport_width),
                drawState.position_mode == 2u,
                drawState.ui_canvas_mode ==
                    XGPU_PLUME_UI_CANVAS_CENTERED,
                transform)) {
            uiCanvasVertices.assign(src, src + sourceByteLength);
            for (uint32_t vertex = 0; vertex < vc; ++vertex) {
                uint8_t *position = uiCanvasVertices.data()
                    + static_cast<size_t>(vertex) * stride;
                float x;
                std::memcpy(&x, position, sizeof(x));
                x = x * transform.scale + transform.offset;
                std::memcpy(position, &x, sizeof(x));
            }
            src = uiCanvasVertices.data();
        }
    }

    /* Snapshot programmable descriptor set at record time so mid-frame
     * SetTexture changes do not rewrite earlier draws. */
    uint32_t psH = 0;
    uint32_t progConstIndex = UINT32_MAX;
    uint32_t vsConstIndex = UINT32_MAX;
    bool activePSValid = false;
    if (m_activePS) {
        auto activeIt = m_psReg.find(m_activePS);
        activePSValid =
            activeIt != m_psReg.end() && activeIt->second.ok;
    }
    const uint32_t requestedPS = plumeSelectDrawPixelShader(
        m_activePS, activePSValid, drawState.z_perspective != 0,
        m_fixedFallbackPS, m_fixedFallbackPSW, m_recordingHostFrame);
    if (m_progReady && requestedPS) {
        XRECOMP_CPU_RECORDER_ZONE_SCOPED("Plume Record Pixel Constants");
        auto psIt = m_psReg.find(requestedPS);
        if (psIt != m_psReg.end() && psIt->second.ok) {
            if (requestedPS == m_fixedFallbackPS ||
                requestedPS == m_fixedFallbackPSW) {
                const std::array<float, 16> textureScales =
                    snapshotProgramTextureScales();
                std::array<float, kProgramConstantFloatCount> constants =
                    snapshotProgramConstants(
                        psIt->second.combinerCB, textureScales, drawState);
                uint32_t writesTexcoordMask = 0;
                const uint32_t tf = drawState.fixed_texture_factor;
                const auto vsIt = m_vsReg.find(vsH);
                if (vsIt != m_vsReg.end()) {
                    for (uint32_t stage = 0; stage < 4; ++stage) {
                        if ((vsIt->second.outputsWritten &
                             (uint16_t)(1u << (NV2A_VSH_OUT_T0 + stage))) != 0)
                            writesTexcoordMask |= 1u << stage;
                    }
                } else if (texCount) {
                    writesTexcoordMask =
                        (1u << std::min<uint32_t>(texCount, 4u)) - 1u;
                }
                for (uint32_t stage = 0; stage < 4; ++stage) {
                    float *color =
                        constants.data() + (8u + stage * 2u) * 4u;
                    float *alpha = color + 4u;
                    color[0] =
                        static_cast<float>(drawState.fixed_color_op[stage]);
                    color[1] =
                        static_cast<float>(drawState.fixed_color_arg0[stage]);
                    color[2] =
                        static_cast<float>(drawState.fixed_color_arg1[stage]);
                    color[3] =
                        static_cast<float>(drawState.fixed_color_arg2[stage]);
                    alpha[0] =
                        static_cast<float>(drawState.fixed_alpha_op[stage]);
                    alpha[1] =
                        static_cast<float>(drawState.fixed_alpha_arg0[stage]);
                    alpha[2] =
                        static_cast<float>(drawState.fixed_alpha_arg1[stage]);
                    alpha[3] =
                        static_cast<float>(drawState.fixed_alpha_arg2[stage]);
                }
                float *factor = constants.data() + 16u * 4u;
                float *metadata = constants.data() + 17u * 4u;
                factor[0] = static_cast<float>((tf >> 16) & 0xFFu) / 255.0f;
                factor[1] = static_cast<float>((tf >> 8) & 0xFFu) / 255.0f;
                factor[2] = static_cast<float>(tf & 0xFFu) / 255.0f;
                factor[3] = static_cast<float>((tf >> 24) & 0xFFu) / 255.0f;
                metadata[0] = static_cast<float>(writesTexcoordMask);
                metadata[1] =
                    drawState.fixed_state_valid ? 1.0f : 0.0f;
                float *alphaTest = constants.data() + 18u * 4u;
                alphaTest[0] =
                    drawState.alpha_test_enable ? 1.0f : 0.0f;
                alphaTest[1] =
                    static_cast<float>(drawState.alpha_func);
                alphaTest[2] =
                    static_cast<float>(drawState.alpha_ref & 0xFFu) / 255.0f;
                progConstIndex = internFrameConstants(
                    m_rec.frameProgConsts,
                    m_rec.frameProgConstBuckets, constants);
            } else {
                progConstIndex = internProgramConstants(
                    psIt->second.combinerCB, drawState);
            }
            psH = requestedPS;
        }
    }
    if (vsH) {
        XRECOMP_CPU_RECORDER_ZONE_SCOPED("Plume Record Vertex Constants");
        const PlumeViewportTransform viewport =
            plume_viewport_transform(drawState);
        std::array<float, 8> viewportKey = {};
        std::memcpy(viewportKey.data(), viewport.scale,
                    sizeof(viewport.scale));
        std::memcpy(viewportKey.data() + 4u, viewport.offset,
                    sizeof(viewport.offset));
        if (!m_rec.frameVSConstCache.lookup(
                m_vsConstVersion, viewportKey, vsConstIndex)) {
            XRECOMP_CPU_RECORDER_ZONE_SCOPED(
                "Plume Build Vertex Constants");
            std::array<float, 768> constants = {};
            std::memcpy(constants.data(), m_vsConst, sizeof(m_vsConst));
            std::memcpy(constants.data() + 58u * 4u, viewport.scale,
                        sizeof(viewport.scale));
            std::memcpy(constants.data() + 59u * 4u, viewport.offset,
                        sizeof(viewport.offset));
            vsConstIndex = internFrameConstants(
                m_rec.frameVSConsts, m_rec.frameVSConstBuckets, constants);
            m_rec.frameVSConstCache.remember(
                m_vsConstVersion, viewportKey, vsConstIndex);
        }
    }


    /* D3D12 has no triangle-fan topology: expand (v0, v(i+1), v(i+2)) to a
     * triangle list. */
    if (topo == (uint8_t)RenderPrimitiveTopology::TRIANGLE_FAN) {
        if (vc < 3) return;
        const uint32_t tris = vc - 2;
        const uint64_t outVc64 = static_cast<uint64_t>(tris) * 3u;
        const uint64_t byteLen64 = outVc64 * stride;
        if (outVc64 > UINT32_MAX || byteLen64 > UINT32_MAX) {
            xgpu_plume_f2_log(
                "skip reason=vertex-stream-overflow kind=triangle-fan "
                "vertices=%llu stride=%u",
                static_cast<unsigned long long>(outVc64), stride);
            return;
        }
        const uint32_t outVc = static_cast<uint32_t>(outVc64);
        const uint32_t byteLen = static_cast<uint32_t>(byteLen64);
        uint32_t off = 0;
        if (!plumeGrowVertexStream(m_rec.frameVerts, byteLen, &off)) {
            xgpu_plume_f2_log(
                "skip reason=vertex-stream-overflow kind=triangle-fan "
                "current=%zu add=%u",
                m_rec.frameVerts.size(), byteLen);
            return;
        }
        uint8_t *dst = m_rec.frameVerts.data() + off;
        for (uint32_t i = 0; i < tris; i++) {
            std::memcpy(dst, src, stride);
            dst += stride;
            std::memcpy(dst, src + (size_t)(i + 1) * stride, stride);
            dst += stride;
            std::memcpy(dst, src + (size_t)(i + 2) * stride, stride);
            dst += stride;
        }
        GeomDraw draw = {};
        draw.offset = off;
        draw.byteLen = byteLen;
        draw.vertexCount = outVc;
        draw.stride = stride;
        draw.fvf = fvf;
        draw.activePsHandle = m_activePS;
        draw.topology = (uint8_t)RenderPrimitiveTopology::TRIANGLE_LIST;
        draw.hasDiffuse = hasDiffuse;
        draw.hasSpecular = hasSpecular;
        draw.uvOffset = uvOffset;
        draw.psHandle = psH;
        draw.progConstIndex = progConstIndex;
        draw.vsHandle = vsH;
        draw.vsConstIndex = vsConstIndex;
        draw.hasUV = hasUV;
        draw.texCount = texCount;
        draw.afterPresentCopy = m_recordingHostOverlay ? 1u : 0u;
        draw.renderState = drawState;
        draw.targetGuest = m_currentTarget;
        draw.targetWidth = m_currentTargetWidth;
        draw.targetHeight = m_currentTargetHeight;
        draw.targetColorGeneration = m_currentColorGeneration;
        draw.targetZetaGeneration = m_currentZetaGeneration;
        draw.targetFramebufferGeneration = m_currentFramebufferGeneration;
        for (uint32_t s = 0; s < 4; s++) {
            draw.surfaceStage[s] = normalizeSampledSurfaceGeneration(
                m_curSurfaceStage[s], draw.targetColorGeneration);
            draw.stageSamplerState[s] = m_curSamplerState[s];
            draw.stageSamplerStateValid[s] = m_curSamplerStateValid[s];
            draw.stageTexture[s] = m_curTextureStage[s];
        }
        draw.recordsZetaWrite =
            drawState.depth_enable && drawState.depth_write ? 1u : 0u;
        draw.recordsColorWrite = 1;
        m_rec.draws.push_back(draw);
        return;
    }

    const uint64_t byteLen64 = static_cast<uint64_t>(vc) * stride;
    if (byteLen64 > UINT32_MAX) {
        xgpu_plume_f2_log(
            "skip reason=vertex-stream-overflow kind=draw "
            "vertices=%u stride=%u",
            vc, stride);
        return;
    }
    const uint32_t byteLen = static_cast<uint32_t>(byteLen64);
    uint32_t off = 0;
    if (!m_omitVertexBytes) {
        XRECOMP_CPU_RECORDER_ZONE_SCOPED("Plume Record Vertex Copy");
        if (!plumeAppendVertexBytes(
                m_rec.frameVerts, src, byteLen, &off)) {
            xgpu_plume_f2_log(
                "skip reason=vertex-stream-overflow kind=draw "
                "current=%zu add=%u",
                m_rec.frameVerts.size(), byteLen);
            return;
        }
    }
    XRECOMP_CPU_RECORDER_ZONE_SCOPED("Plume Record Metadata");
    GeomDraw draw = {};
    draw.offset = off;
    draw.byteLen = byteLen;
    draw.vertexCount = vc;
    draw.stride = stride;
    draw.fvf = fvf;
    draw.activePsHandle = m_activePS;
    draw.topology = topo;
    draw.hasDiffuse = hasDiffuse;
    draw.hasSpecular = hasSpecular;
    draw.uvOffset = uvOffset;
    draw.psHandle = psH;
    draw.progConstIndex = progConstIndex;
    draw.vsHandle = vsH;
    draw.vsConstIndex = vsConstIndex;
    draw.hasUV = hasUV;
    draw.texCount = texCount;
    draw.afterPresentCopy = m_recordingHostOverlay ? 1u : 0u;
    draw.renderState = drawState;
    draw.targetGuest = m_currentTarget;
    draw.targetWidth = m_currentTargetWidth;
    draw.targetHeight = m_currentTargetHeight;
    draw.targetColorGeneration = m_currentColorGeneration;
    draw.targetZetaGeneration = m_currentZetaGeneration;
    draw.targetFramebufferGeneration = m_currentFramebufferGeneration;
    for (uint32_t s = 0; s < 4; s++) {
        draw.surfaceStage[s] = normalizeSampledSurfaceGeneration(
            m_curSurfaceStage[s], draw.targetColorGeneration);
        draw.stageSamplerState[s] = m_curSamplerState[s];
        draw.stageSamplerStateValid[s] = m_curSamplerStateValid[s];
        draw.stageTexture[s] = m_curTextureStage[s];
    }
    draw.recordsZetaWrite =
        drawState.depth_enable && drawState.depth_write ? 1u : 0u;
    draw.recordsColorWrite = 1;
    m_rec.draws.push_back(draw);
}

bool PlumeDraw::recordIndexedDraw(
    PlumeContext &ctx, uint32_t primType, uint32_t primCount,
    const void *verts, uint32_t vertexCount,
    uint32_t stride, uint32_t fvf,
    const uint32_t *indices, uint32_t indexCount,
    const XgpuPlumeRenderState *renderState)
{
    uint32_t expectedIndexCount;
    uint8_t topology;

    if (!verts || !vertexCount || !stride || !indices || !indexCount ||
        m_rec.frameIndices.size() > UINT32_MAX ||
        indexCount > UINT32_MAX - m_rec.frameIndices.size())
        return false;
    switch ((D3DPRIMITIVETYPE)primType) {
    case D3DPT_POINTLIST:
        expectedIndexCount = primCount;
        topology = (uint8_t)RenderPrimitiveTopology::POINT_LIST;
        break;
    case D3DPT_LINELIST:
        if (primCount > UINT32_MAX / 2u)
            return false;
        expectedIndexCount = primCount * 2u;
        topology = (uint8_t)RenderPrimitiveTopology::LINE_LIST;
        break;
    case D3DPT_LINESTRIP:
        if (primCount == UINT32_MAX)
            return false;
        expectedIndexCount = primCount + 1u;
        topology = (uint8_t)RenderPrimitiveTopology::LINE_STRIP;
        break;
    case D3DPT_TRIANGLELIST:
        if (primCount > UINT32_MAX / 3u)
            return false;
        expectedIndexCount = primCount * 3u;
        topology = (uint8_t)RenderPrimitiveTopology::TRIANGLE_LIST;
        break;
    case D3DPT_TRIANGLESTRIP:
        if (primCount > UINT32_MAX - 2u)
            return false;
        expectedIndexCount = primCount + 2u;
        topology = (uint8_t)RenderPrimitiveTopology::TRIANGLE_STRIP;
        break;
    default:
        return false;
    }
    if (expectedIndexCount != indexCount)
        return false;

    const size_t drawCount = m_rec.draws.size();
    /* Reuse ordinary draw recording for immutable vertex bytes and the exact
     * fixed/program state snapshot. POINTLIST makes its source count equal to
     * the contiguous vertex range; the recorded topology is replaced below. */
    recordDraw(ctx, D3DPT_POINTLIST, vertexCount, verts, stride, fvf,
               renderState);
    if (m_rec.draws.size() != drawCount + 1u)
        return false;

    GeomDraw &draw = m_rec.draws.back();
    draw.topology = topology;
    draw.indexOffset = m_omitVertexBytes
        ? 0u : (uint32_t)m_rec.frameIndices.size();
    draw.indexCount = indexCount;
    if (!m_omitVertexBytes)
        m_rec.frameIndices.insert(m_rec.frameIndices.end(), indices,
                                  indices + indexCount);
    return true;
}

bool PlumeDraw::recordCachedIndexedDraw(PlumeContext &ctx,
                                        const XgpuPlumeCachedIndexedDraw &draw)
{
    if (!draw.vertices || !draw.indices || !draw.stride ||
        !draw.index_count || !draw.vertex_count ||
        draw.prim_type != D3DPT_TRIANGLELIST)
        return false;
    if (draw.prim_count > UINT32_MAX / 3u ||
        draw.prim_count * 3u != draw.index_count)
        return false;
    if (draw.vertex_count > draw.index_count)
        return false;

    uint32_t minimum = UINT32_MAX;
    uint32_t maximum = 0;
    for (uint32_t i = 0; i < draw.index_count; ++i) {
        uint32_t index = draw.indices[i];
        if (index < minimum)
            minimum = index;
        if (index > maximum)
            maximum = index;
    }
    if (minimum == UINT32_MAX || maximum < minimum)
        return false;
    const uint32_t uniqueCount = maximum - minimum + 1u;
    if (uniqueCount > draw.vertex_count)
        return false;

    PlumeMeshCacheKey key = {};
    key.vb_data_va = draw.vb_data_va;
    key.ib_data_va = draw.ib_data_va;
    key.index_byte_offset = draw.index_byte_offset;
    key.index_count = draw.index_count;
    key.stride = draw.stride;
    key.base_vertex = draw.base_vertex;
    key.fvf_or_vs = draw.fvf_or_vs;

    uint32_t id = plume_mesh_cache_find(&key, draw.vb_generation,
                                        draw.ib_generation);
    std::vector<uint32_t> remapped(draw.index_count);
    for (uint32_t i = 0; i < draw.index_count; ++i)
        remapped[i] = (uint32_t)draw.indices[i] - minimum;
    const uint8_t *uniqueVerts =
        static_cast<const uint8_t *>(draw.vertices) +
        (size_t)(minimum - draw.base_vertex) * draw.stride;
    const uint32_t uniqueBytes = uniqueCount * draw.stride;

    if (!id) {
        id = plume_mesh_cache_store(&key, draw.vb_generation,
                                    draw.ib_generation, uniqueCount,
                                    draw.index_count);
        if (!id)
            return false;
        if (m_cachedMeshes.size() <= id)
            m_cachedMeshes.resize(id + 1u);
        CachedMeshGpu &mesh = m_cachedMeshes[id];
        mesh.vb = ctx.device()->createBuffer(
            RenderBufferDesc::VertexBuffer(
                uniqueBytes, RenderHeapType::UPLOAD));
        mesh.ib = ctx.device()->createBuffer(
            RenderBufferDesc::IndexBuffer(
                (size_t)draw.index_count * sizeof(uint32_t),
                RenderHeapType::UPLOAD));
        if (!mesh.vb || !mesh.ib ||
            !copyToMappedBuffer(mesh.vb.get(), uniqueVerts, uniqueBytes) ||
            !copyToMappedBuffer(mesh.ib.get(), remapped.data(),
                                remapped.size() * sizeof(uint32_t))) {
            mesh = CachedMeshGpu();
            return false;
        }
        mesh.vbBytes = uniqueBytes;
        mesh.indexCount = draw.index_count;
        mesh.stride = draw.stride;
        m_cachedMisses++;
    } else {
        m_cachedHits++;
        m_cachedBytesSaved += uniqueBytes;
        m_cachedBytesSaved +=
            (uint64_t)draw.index_count * sizeof(uint32_t);
    }

    m_omitVertexBytes = true;
    bool recorded = recordIndexedDraw(
        ctx, draw.prim_type, draw.prim_count, uniqueVerts, uniqueCount,
        draw.stride, draw.fvf_or_vs, remapped.data(), draw.index_count,
        draw.render_state);
    m_omitVertexBytes = false;
    if (!recorded || m_rec.draws.empty())
        return false;
    GeomDraw &geom = m_rec.draws.back();
    geom.cachedMeshId = id;
    geom.offset = 0;
    geom.byteLen = uniqueBytes;
    geom.indexOffset = 0;
    geom.indexCount = draw.index_count;
    static int logged;
    if (!logged) {
        std::fprintf(stderr,
                     "[PLUME-MESH] persistent indexed path active\n");
        logged = 1;
    }
    return true;
}

void PlumeDraw::meshCacheInvalidateVa(uint32_t resource_data_va)
{
    uint32_t dropped[64];
    size_t count = 64;
    size_t i;

    if (!resource_data_va)
        return;
    plume_mesh_cache_invalidate_resource(resource_data_va, dropped, &count);
    for (i = 0; i < count; ++i) {
        uint32_t id = dropped[i];
        if (id < m_cachedMeshes.size())
            m_cachedMeshes[id] = CachedMeshGpu();
    }
}

/* NV2A vertex attribute format -> Plume input-layout format. Must match the
 * HLSL input types emitted by d3d8_vsh (vsh_raw_input): F/UB/S1 auto-convert to
 * float4; S32K -> SINT (HLSL int + cast); CMP -> R32_SINT (HLSL unpack). */
static ::plume::RenderFormat nv2a_attr_render_format(uint32_t format)
{
    using RF = ::plume::RenderFormat;
    uint32_t data_type = format & 0xFFu;
    uint32_t type = format & 0xFu;
    uint32_t count = (format >> 4) & 0xFu;
    switch (data_type) {
    case 0x14u: return RF::R8_UNORM;             /* PBYTE1 */
    case 0x24u: return RF::R8G8_UNORM;           /* PBYTE2 */
    case 0x34u: return RF::R8G8B8A8_UNORM;       /* PBYTE3, .w ignored */
    case 0x44u: return RF::R8G8B8A8_UNORM;       /* PBYTE4 */
    case 0x72u: return RF::R32G32B32_FLOAT;       /* FLOAT2H: x,y,w */
    default: break;
    }
    switch (type) {
    case 2: /* F */
        return count >= 4 ? RF::R32G32B32A32_FLOAT
             : count == 3 ? RF::R32G32B32_FLOAT
             : count == 2 ? RF::R32G32_FLOAT : RF::R32_FLOAT;
    case 0: return RF::B8G8R8A8_UNORM; /* UB_D3D (BGRA) */
    case 4: return RF::R8G8B8A8_UNORM; /* UB_OGL (RGBA) */
    case 1: /* S1: normalized short (count 3 over-reads into .w) */
        return count >= 3 ? RF::R16G16B16A16_SNORM
             : count == 2 ? RF::R16G16_SNORM : RF::R16_SNORM;
    case 5: /* S32K: raw short (count 3 over-reads into .w) */
        return count >= 3 ? RF::R16G16B16A16_SINT
             : count == 2 ? RF::R16G16_SINT : RF::R16_SINT;
    case 6: return RF::R32_SINT; /* CMP */
    default: return RF::UNKNOWN;
    }
}

static uint32_t nv2a_attr_host_size(uint32_t format)
{
    switch (format & 0xFFu) {
    case 0x12u: return 4;
    case 0x22u: return 8;
    case 0x32u: return 12;
    case 0x42u: return 16;
    case 0x40u: return 4;
    case 0x25u: return 4;
    case 0x45u: return 8;
    case 0x11u: return 2;
    case 0x21u: return 4;
    case 0x31u: return 8;  /* no three-component DXGI short format */
    case 0x41u: return 8;
    case 0x16u: return 4;
    case 0x15u: return 2;
    case 0x35u: return 8;  /* no three-component DXGI short format */
    case 0x14u: return 1;
    case 0x24u: return 2;
    case 0x34u: return 4;  /* no three-component DXGI byte format */
    case 0x44u: return 4;
    case 0x72u: return 12;
    default: return 0;
    }
}

::plume::RenderPipeline *PlumeDraw::progIdxPso(PlumeContext &ctx,
                                              const ProgDraw &d,
                                              bool hasDepthAttachment)
{
    auto vertexProgram = m_vpCache.find(d.vertexProgramKey);
    RenderShader *vs =
        vertexProgram != m_vpCache.end() && vertexProgram->second.ok
            ? vertexProgram->second.shader.get() : nullptr;
    uint64_t key = d.vertexProgramKey ^ ((uint64_t)d.psHandle << 1) ^
                   ((uint64_t)d.topology << 40) ^
                   plume_render_state_key(d.renderState);
    key = (key * 1099511628211ull) ^ (hasDepthAttachment ? 1ull : 0ull);
    for (int i = 0; i < 16; i++)
        if (d.attrsUsed & (1u << i))
            key = key * 1099511628211ull ^
                  (d.attrFormat[i] + ((uint64_t)d.attrOffset[i] << 20));
    auto it = m_progIdxPsos.find(key);
    if (it != m_progIdxPsos.end())
        return it->second.get();

    RenderShader *ps = this->progPixelShader(ctx, d.psHandle);
    if (!ps || !vs)
        return nullptr;

    RenderInputSlot slot(0, d.stride);
    RenderInputElement elems[16];
    uint32_t ne = 0;
    for (int i = 0; i < 16; i++) {
        if (!(d.attrsUsed & (1u << i)))
            continue;
        RenderFormat fmt = nv2a_attr_render_format(d.attrFormat[i]);
        if (fmt == RenderFormat::UNKNOWN)
            return nullptr; /* unsupported format -> caller uses CPU path */
        elems[ne] = RenderInputElement("ATTR", (uint32_t)i, ne, fmt, 0,
                                       d.attrOffset[i]);
        ne++;
    }
    if (ne == 0)
        return nullptr;

    RenderGraphicsPipelineDesc pd;
    pd.pipelineLayout = m_progIdxLayout.get();
    pd.vertexShader = vs;
    pd.pixelShader = ps;
    pd.inputSlots = &slot;
    pd.inputSlotsCount = 1;
    pd.inputElements = elems;
    pd.inputElementsCount = ne;
    pd.renderTargetFormat[0] = RenderFormat::B8G8R8A8_UNORM;
    pd.renderTargetCount = 1;
    pd.primitiveTopology = (RenderPrimitiveTopology)d.topology;
    plume_draw_apply_state(pd, d.renderState, hasDepthAttachment);

    XRECOMP_TRACY_ZONE_SCOPED("Plume Create Graphics Pipeline");
    uint64_t perf_pipeline_t0 = xgpu_plume_perf_begin();
    std::unique_ptr<RenderPipeline> pso =
        ctx.device()->createGraphicsPipeline(pd);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_PIPELINE, perf_pipeline_t0);
    RenderPipeline *raw = pso.get();
    m_progIdxPsos.emplace(key, std::move(pso));
    return raw;
}

XgpuPlumeGpuDrawResult PlumeDraw::recordProgIndexedDraw(
    PlumeContext &ctx, const XgpuProgIndexedDraw &desc)
{
    XRECOMP_TRACY_ZONE_SCOPED("Plume Record Indexed Draw");
    (void)ctx;
    /* Need a translated GPU vertex shader and an active combiner pixel shader;
     * otherwise the caller must fall back to the CPU interpreter. */
    if (!m_curVertexProgramKey || !m_progReady || !m_activePS)
        return XGPU_PLUME_GPU_FALLBACK_SHADER;
    if (!desc.vertex_data || !desc.indices || desc.index_count < 3 ||
        !desc.stride || !desc.vs_constants || !desc.attr_format ||
        !desc.attr_offset)
        return XGPU_PLUME_GPU_FALLBACK_RECORD_REJECTION;
    auto psIt = m_psReg.find(m_activePS);
    if (psIt == m_psReg.end() || !psIt->second.ok)
        return XGPU_PLUME_GPU_FALLBACK_SHADER;
    /* Inputs not backed by a vertex array retain their persistent Xbox v#
     * latch. Materialize those values as appended float4 attributes so the
     * translated program can still execute on the portable GPU path. */
    const uint16_t latchInputs =
        (uint16_t)(m_curVertexInputs & (uint16_t)~desc.attrs_used);
    if (latchInputs && !desc.latched_inputs)
        return XGPU_PLUME_GPU_FALLBACK_LATCH_INPUT;
    const uint16_t fetchInputs =
        (uint16_t)(desc.attrs_used & m_curVertexInputs);
    if (!(fetchInputs | latchInputs))
        return XGPU_PLUME_GPU_FALLBACK_FORMAT;

    ProgDraw d = {};
    d.afterGeom = (uint32_t)m_rec.draws.size();
    d.psHandle = m_activePS;
    d.vertexProgramKey = m_curVertexProgramKey;
    d.stride = desc.stride;
    d.indexCount = desc.index_count;
    d.viewportZOffset = desc.viewport_z_offset;
    d.viewportZScale = desc.viewport_z_scale;
    /* Input layout must match VS_IN exactly (declared only for read inputs):
     * arrayed attributes the program does not read are not fetched, and an
     * unsupported format on such an attribute must not fail the PSO. */
    d.attrsUsed = (uint16_t)(fetchInputs | latchInputs);
    for (int i = 0; i < 16; i++) {
        d.attrFormat[i] = desc.attr_format[i];
        d.attrOffset[i] = desc.attr_offset[i];
    }
    switch ((D3DPRIMITIVETYPE)desc.prim_type) {
    case D3DPT_TRIANGLESTRIP:
        d.topology = (uint8_t)RenderPrimitiveTopology::TRIANGLE_STRIP;
        break;
    case D3DPT_TRIANGLEFAN:
        d.topology = (uint8_t)RenderPrimitiveTopology::TRIANGLE_FAN;
        break;
    default: d.topology = (uint8_t)RenderPrimitiveTopology::TRIANGLE_LIST; break;
    }
    d.renderState = desc.render_state ? *desc.render_state
                                      : XgpuPlumeRenderState{};
    d.renderState.zeta_format = m_currentZetaFormat;
    d.renderState.zeta_float = m_currentZetaFloat;
    d.targetGuest = m_currentTarget;
    d.targetWidth = m_currentTargetWidth;
    d.targetHeight = m_currentTargetHeight;
    d.targetColorGeneration = m_currentColorGeneration;
    d.targetZetaGeneration = m_currentZetaGeneration;
    d.targetFramebufferGeneration = m_currentFramebufferGeneration;
    d.texCount = 0;
    for (uint32_t s = 0; s < 4; s++) {
        d.surfaceStage[s] = normalizeSampledSurfaceGeneration(
            m_curSurfaceStage[s], d.targetColorGeneration);
        d.stageSamplerState[s] = m_curSamplerState[s];
        d.stageSamplerStateValid[s] = m_curSamplerStateValid[s];
        d.stageTexture[s] = m_curTextureStage[s];
        if (m_curTextureStage[s].valid() || m_curSurfaceStage[s])
            d.texCount = (uint8_t)(s + 1);
    }

    /* Stage raw vertex bytes, indices, the portable VS constant stream (t4),
     * and combiner PS constants (b8). Latch draws are expanded into per-index
     * vertices because the persistent v# values belong to this draw, not the
     * shared DMA array. */
    if (latchInputs) {
        LatchedVertexStream stream = {};
        if (!materializeLatchedVertexStream(
                m_rec.frameRawVerts, m_rec.frameIndices, desc.vertex_data,
                desc.vertex_bytes, desc.stride, desc.indices,
                desc.index_count, latchInputs, desc.latched_inputs, &stream))
            return XGPU_PLUME_GPU_FALLBACK_RECORD_REJECTION;
        d.vbufOffset = stream.vertexOffset;
        d.vbufBytes = stream.vertexBytes;
        d.stride = stream.stride;
        d.ibufOffset = stream.indexOffset;
        d.indexCount = stream.indexCount;
        for (uint32_t attr = 0; attr < 16; attr++) {
            if (!(latchInputs & (uint16_t)(1u << attr)))
                continue;
            d.attrFormat[attr] = 2u | (4u << 4); /* NV2A float4 */
            d.attrOffset[attr] = stream.latchOffsets[attr];
        }
    } else {
        /* A few trailing pad bytes cover the over-read of a 4th short for
         * 3-component S32K/S1 attributes. */
        size_t vbo = (m_rec.frameRawVerts.size() + 15u) & ~size_t(15u);
        m_rec.frameRawVerts.resize(vbo);
        d.vbufOffset = (uint32_t)vbo;
        d.vbufBytes = desc.vertex_bytes;
        const uint8_t *vsrc = (const uint8_t *)desc.vertex_data;
        m_rec.frameRawVerts.insert(
            m_rec.frameRawVerts.end(), vsrc, vsrc + desc.vertex_bytes);
        m_rec.frameRawVerts.resize(m_rec.frameRawVerts.size() + 8u, 0u);

        d.ibufOffset = (uint32_t)m_rec.frameIndices.size();
        m_rec.frameIndices.insert(m_rec.frameIndices.end(), desc.indices,
                              desc.indices + desc.index_count);
    }

    std::array<float, 768> vsConst = {};
    std::memcpy(vsConst.data(), desc.vs_constants, sizeof(vsConst));
    d.vsConstIndex = internFrameConstants(
        m_rec.frameVSConsts, m_rec.frameVSConstBuckets, vsConst);

    d.progConstIndex = internProgramConstants(
        psIt->second.combinerCB, d.renderState);

    d.recordsZetaWrite =
        d.renderState.depth_enable && d.renderState.depth_write ? 1u : 0u;
    d.recordsColorWrite = 1;
    m_rec.progDraws.push_back(d);
    return XGPU_PLUME_GPU_ACCEPTED;
}

bool PlumeDraw::queueHostFrame(PlumeContext &ctx, const void *pixels, uint32_t w,
                               uint32_t h, uint32_t pitch, uint32_t format,
                               uint64_t version)
{
    if (!pixels || !w || !h || !pitch)
        return false;
    if (format != kHostFrameFormat && format != 0x24u && format != 0x25u)
        return false;

    const uint64_t byteCount = (uint64_t)pitch * h;
    if (byteCount > UINT32_MAX)
        return false;

    setTexture(0, kHostFrameGuest, pixels, w, h, pitch,
               (uint32_t)byteCount, format, version);
    if (!m_curTextureStage[0].valid())
        return false;

    /* Keep host-decoded video at its native pixel extent and center it in the
     * active output. Widescreen changes the game view, not movie geometry. */
    const std::array<PlumeOutputQuadVertex, 6> vertices =
        plumeCenteredOutputQuadVertices(w, h, m_outputWidth, m_outputHeight);
    const uint32_t activePS = m_activePS;
    const uint32_t activeVS = m_activeVS;
    const uint32_t target = m_currentTarget;
    const uint32_t targetWidth = m_currentTargetWidth;
    const uint32_t targetHeight = m_currentTargetHeight;
    const size_t drawCount = m_rec.draws.size();
    const uint64_t colorGeneration = m_currentColorGeneration;
    const uint64_t zetaGeneration = m_currentZetaGeneration;
    const uint64_t framebufferGeneration = m_currentFramebufferGeneration;
    const uint32_t zetaFormat = m_currentZetaFormat;
    const uint32_t zetaFloat = m_currentZetaFloat;
    m_activePS = 0;
    m_activeVS = 0;
    m_currentTarget = 0;
    m_currentTargetWidth = m_outputWidth;
    m_currentTargetHeight = m_outputHeight;
    m_currentColorGeneration = 0;
    m_currentZetaGeneration = 0;
    m_currentFramebufferGeneration = 0;
    m_currentZetaFormat = XGPU_ZETA_NONE;
    m_currentZetaFloat = 0;
    m_presentTarget = 0;
    m_recordingHostFrame = true;
    m_stickyHostFrame = true;
    recordDraw(ctx, D3DPT_TRIANGLELIST, 2, vertices.data(),
               sizeof(vertices[0]), 0, nullptr);
    m_recordingHostFrame = false;
    m_activePS = activePS;
    m_activeVS = activeVS;
    m_currentTarget = target;
    m_currentTargetWidth = targetWidth;
    m_currentTargetHeight = targetHeight;
    m_currentColorGeneration = colorGeneration;
    m_currentZetaGeneration = zetaGeneration;
    m_currentFramebufferGeneration = framebufferGeneration;
    m_currentZetaFormat = zetaFormat;
    m_currentZetaFloat = zetaFloat;
    return m_rec.draws.size() > drawCount;
}

bool PlumeDraw::queueHostOverlay(
    PlumeContext &ctx,
    const XgpuPlumeDebugOverlayFrame &frame,
    uint32_t slot)
{
    if (!plumeDebugOverlayFrameValid(
            frame, XGPU_PANEL_WIDTH, XGPU_PANEL_HEIGHT))
        return false;
    if (slot >= XGPU_PLUME_MAX_DEBUG_OVERLAY_PROVIDERS)
        return false;
    const uint64_t byteCount =
        static_cast<uint64_t>(frame.pitch) * frame.height;
    if (byteCount > UINT32_MAX)
        return false;

    const RecordedTextureBinding savedStage0 = m_curTextureStage[0];
    const uint64_t savedSurfaceStage0 = m_curSurfaceStage[0];
    const uint8_t savedSurfaceUnnormalized0 = m_curSurfaceUnnormalized[0];
    const uint32_t savedActivePS = m_activePS;
    const uint32_t savedActiveVS = m_activeVS;
    const uint32_t savedTarget = m_currentTarget;
    const uint32_t savedTargetWidth = m_currentTargetWidth;
    const uint32_t savedTargetHeight = m_currentTargetHeight;
    const uint64_t savedColorGeneration = m_currentColorGeneration;
    const uint64_t savedZetaGeneration = m_currentZetaGeneration;
    const uint64_t savedFramebufferGeneration = m_currentFramebufferGeneration;
    const uint32_t savedZetaFormat = m_currentZetaFormat;
    const uint32_t savedZetaFloat = m_currentZetaFloat;
    const uint32_t savedPresentTarget = m_presentTarget;
    const bool savedRecordingHostFrame = m_recordingHostFrame;
    const bool savedRecordingHostOverlay = m_recordingHostOverlay;
    const bool savedStickyHostFrame = m_stickyHostFrame;
    const size_t drawCount = m_rec.draws.size();

    setTexture(0, plumeDebugOverlayTextureGuest(slot), frame.pixels,
               frame.width, frame.height, frame.pitch,
               static_cast<uint32_t>(byteCount), kHostFrameFormat,
               frame.version);
    if (m_curTextureStage[0].valid()) {
        const std::array<PlumeDebugOverlayVertex, 6> vertices =
            plumeDebugOverlayVertices(frame);
        XgpuPlumeRenderState state = plume_draw_default_state();
        state.depth_enable = 0;
        state.depth_write = 0;
        state.cull_mode = 1;

        m_activePS = 0;
        m_activeVS = 0;
        m_currentTarget = 0;
        m_currentTargetWidth = XGPU_PANEL_WIDTH;
        m_currentTargetHeight = XGPU_PANEL_HEIGHT;
        m_currentColorGeneration = 0;
        m_currentZetaGeneration = 0;
        m_currentFramebufferGeneration = 0;
        m_currentZetaFormat = XGPU_ZETA_NONE;
        m_currentZetaFloat = 0;
        m_presentTarget = 0;
        m_recordingHostFrame = true;
        m_recordingHostOverlay = true;
        recordDraw(ctx, D3DPT_TRIANGLELIST, 2, vertices.data(),
                   sizeof(vertices[0]), 0, &state);
    }

    m_curTextureStage[0] = savedStage0;
    m_curSurfaceStage[0] = savedSurfaceStage0;
    m_curSurfaceUnnormalized[0] = savedSurfaceUnnormalized0;
    m_activePS = savedActivePS;
    m_activeVS = savedActiveVS;
    m_currentTarget = savedTarget;
    m_currentTargetWidth = savedTargetWidth;
    m_currentTargetHeight = savedTargetHeight;
    m_currentColorGeneration = savedColorGeneration;
    m_currentZetaGeneration = savedZetaGeneration;
    m_currentFramebufferGeneration = savedFramebufferGeneration;
    m_currentZetaFormat = savedZetaFormat;
    m_currentZetaFloat = savedZetaFloat;
    m_presentTarget = savedPresentTarget;
    m_recordingHostFrame = savedRecordingHostFrame;
    m_recordingHostOverlay = savedRecordingHostOverlay;
    m_stickyHostFrame = savedStickyHostFrame;
    return m_rec.draws.size() > drawCount;
}

bool PlumeDraw::queueHostOutputOverlay(
    PlumeContext &ctx,
    const XgpuPlumeDebugOverlayFrame &frame,
    uint32_t slot)
{
    (void)ctx;
    if (slot >= XGPU_PLUME_MAX_DEBUG_OVERLAY_PROVIDERS)
        return false;
    const uint64_t byteCount =
        static_cast<uint64_t>(frame.pitch) * frame.height;
    if (byteCount > UINT32_MAX)
        return false;

    const RecordedTextureBinding savedStage0 = m_curTextureStage[0];
    const uint64_t savedSurfaceStage0 = m_curSurfaceStage[0];
    const uint8_t savedSurfaceUnnormalized0 = m_curSurfaceUnnormalized[0];
    setTexture(0, plumeDebugOverlayTextureGuest(slot), frame.pixels,
               frame.width, frame.height, frame.pitch,
               static_cast<uint32_t>(byteCount), kHostFrameFormat,
               frame.version);
    if (m_curTextureStage[0].valid()) {
        HostOutputOverlay overlay;
        overlay.binding = m_curTextureStage[0];
        overlay.x = frame.x;
        overlay.y = frame.y;
        overlay.width = frame.width;
        overlay.height = frame.height;
        m_hostOutputOverlays.push_back(overlay);
    }
    m_curTextureStage[0] = savedStage0;
    m_curSurfaceStage[0] = savedSurfaceStage0;
    m_curSurfaceUnnormalized[0] = savedSurfaceUnnormalized0;
    return !m_hostOutputOverlays.empty();
}

void PlumeDraw::ensureStickyHostFrame(PlumeContext &ctx)
{
    if (!m_stickyHostFrame || !m_texReady)
        return;

    auto *entry = m_textures.current(kHostFrameGuest);
    if (!entry || !entry->resource.descSet) {
        m_stickyHostFrame = false;
        return;
    }

    m_curTextureStage[0] = entry->binding;

    const std::array<PlumeOutputQuadVertex, 6> vertices =
        plumeCenteredOutputQuadVertices(
            entry->resource.w, entry->resource.h,
            m_outputWidth, m_outputHeight);
    const uint32_t activePS = m_activePS;
    const uint32_t activeVS = m_activeVS;
    const uint32_t target = m_currentTarget;
    const uint32_t targetWidth = m_currentTargetWidth;
    const uint32_t targetHeight = m_currentTargetHeight;
    const uint64_t colorGeneration = m_currentColorGeneration;
    const uint64_t zetaGeneration = m_currentZetaGeneration;
    const uint64_t framebufferGeneration = m_currentFramebufferGeneration;
    const uint32_t zetaFormat = m_currentZetaFormat;
    const uint32_t zetaFloat = m_currentZetaFloat;
    m_activePS = 0;
    m_activeVS = 0;
    m_currentTarget = 0;
    m_currentTargetWidth = m_outputWidth;
    m_currentTargetHeight = m_outputHeight;
    m_currentColorGeneration = 0;
    m_currentZetaGeneration = 0;
    m_currentFramebufferGeneration = 0;
    m_currentZetaFormat = XGPU_ZETA_NONE;
    m_currentZetaFloat = 0;
    m_presentTarget = 0;
    m_recordingHostFrame = true;
    recordDraw(ctx, D3DPT_TRIANGLELIST, 2, vertices.data(),
               sizeof(vertices[0]), 0, nullptr);
    m_recordingHostFrame = false;
    m_activePS = activePS;
    m_activeVS = activeVS;
    m_currentTarget = target;
    m_currentTargetWidth = targetWidth;
    m_currentTargetHeight = targetHeight;
    m_currentColorGeneration = colorGeneration;
    m_currentZetaGeneration = zetaGeneration;
    m_currentFramebufferGeneration = framebufferGeneration;
    m_currentZetaFormat = zetaFormat;
    m_currentZetaFloat = zetaFloat;
}

void PlumeDraw::ensurePresentSurfaceComposite(PlumeContext &ctx)
{
    if (!m_presentTarget || m_stickyHostFrame)
        return;

    auto latest = m_latestSurfaceGeneration.find(m_presentTarget);
    auto surface = latest != m_latestSurfaceGeneration.end()
        ? m_surfaceCache.find(latest->second) : m_surfaceCache.end();
    if (surface == m_surfaceCache.end() ||
        !plumePresentSurfaceNeedsComposite(
            surface->second.width, surface->second.height,
            m_outputWidth, m_outputHeight)) {
        return;
    }

    const RecordedTextureBinding savedStage0 = m_curTextureStage[0];
    const uint64_t savedSurfaceStage0 = m_curSurfaceStage[0];
    const uint8_t savedSurfaceUnnormalized0 = m_curSurfaceUnnormalized[0];
    const uint32_t savedActivePS = m_activePS;
    const uint32_t savedActiveVS = m_activeVS;
    const uint32_t savedTarget = m_currentTarget;
    const uint32_t savedTargetWidth = m_currentTargetWidth;
    const uint32_t savedTargetHeight = m_currentTargetHeight;
    const uint64_t savedColorGeneration = m_currentColorGeneration;
    const uint64_t savedZetaGeneration = m_currentZetaGeneration;
    const uint64_t savedFramebufferGeneration = m_currentFramebufferGeneration;
    const uint32_t savedZetaFormat = m_currentZetaFormat;
    const uint32_t savedZetaFloat = m_currentZetaFloat;
    const bool savedRecordingHostFrame = m_recordingHostFrame;

    const std::array<PlumeOutputQuadVertex, 6> vertices =
        plumeOutputQuadVertices(m_outputWidth, m_outputHeight);
    XgpuPlumeRenderState state = plume_draw_default_state();
    state.depth_enable = 0;
    state.depth_write = 0;
    state.cull_mode = 1;

    m_curTextureStage[0] = {};
    m_curSurfaceStage[0] = latest->second;
    m_curSurfaceUnnormalized[0] = 0;
    m_activePS = 0;
    m_activeVS = 0;
    m_currentTarget = 0;
    m_currentTargetWidth = m_outputWidth;
    m_currentTargetHeight = m_outputHeight;
    m_currentColorGeneration = 0;
    m_currentZetaGeneration = 0;
    m_currentFramebufferGeneration = 0;
    m_currentZetaFormat = XGPU_ZETA_NONE;
    m_currentZetaFloat = 0;
    m_recordingHostFrame = true;
    recordDraw(ctx, D3DPT_TRIANGLELIST, 2, vertices.data(),
               sizeof(vertices[0]), 0, &state);

    m_curTextureStage[0] = savedStage0;
    m_curSurfaceStage[0] = savedSurfaceStage0;
    m_curSurfaceUnnormalized[0] = savedSurfaceUnnormalized0;
    m_activePS = savedActivePS;
    m_activeVS = savedActiveVS;
    m_currentTarget = savedTarget;
    m_currentTargetWidth = savedTargetWidth;
    m_currentTargetHeight = savedTargetHeight;
    m_currentColorGeneration = savedColorGeneration;
    m_currentZetaGeneration = savedZetaGeneration;
    m_currentFramebufferGeneration = savedFramebufferGeneration;
    m_currentZetaFormat = savedZetaFormat;
    m_currentZetaFloat = savedZetaFloat;
    m_recordingHostFrame = savedRecordingHostFrame;
}


uint32_t PlumeDraw::drawsToPresentSurface() const
{
    if (!m_presentTarget)
        return 0;
    uint32_t n = 0;
    for (const GeomDraw &d : m_rec.draws) {
        if (d.targetGuest == m_presentTarget)
            n++;
    }
    for (const ProgDraw &d : m_rec.progDraws) {
        if (d.targetGuest == m_presentTarget)
            n++;
    }
    return n;
}

PlumeDraw::F2QueueFingerprint PlumeDraw::captureF2QueueFingerprint() const
{
    F2QueueFingerprint fp = {};
    fp.draw_hash = 1469598103934665603ull;

    for (const GeomDraw &d : m_rec.draws) {
        if (d.clear || d.clearDepth || d.clearStencil) {
            fp.draw_hash ^= 0x434C45A1u; /* clear-op tag */
            fp.draw_hash *= 1099511628211ull;
            fp.draw_hash ^= d.targetGuest;
            fp.draw_hash *= 1099511628211ull;
            continue;
        }
        if (d.targetGuest && d.targetGuest == m_presentTarget)
            fp.draws_to_present++;
        fp.draw_hash ^= d.targetGuest;
        fp.draw_hash *= 1099511628211ull;
        fp.draw_hash ^= d.vertexCount;
        fp.draw_hash *= 1099511628211ull;
        fp.draw_hash ^= d.indexCount;
        fp.draw_hash *= 1099511628211ull;
        fp.draw_hash ^= d.psHandle;
        fp.draw_hash *= 1099511628211ull;
        fp.draw_hash ^= (uint32_t)d.hasDiffuse | ((uint32_t)d.hasSpecular << 1) |
                        ((uint32_t)d.hasUV << 2) |
                        ((uint32_t)d.renderState.blend_enable << 3);
        fp.draw_hash *= 1099511628211ull;
    }
    for (const ProgDraw &d : m_rec.progDraws) {
        if (d.targetGuest && d.targetGuest == m_presentTarget)
            fp.draws_to_present++;
        fp.draw_hash ^= 0x50524F47u; /* indexed programmable-draw tag */
        fp.draw_hash *= 1099511628211ull;
        fp.draw_hash ^= d.targetGuest;
        fp.draw_hash *= 1099511628211ull;
        fp.draw_hash ^= d.indexCount;
        fp.draw_hash *= 1099511628211ull;
        fp.draw_hash ^= d.psHandle;
        fp.draw_hash *= 1099511628211ull;
        fp.draw_hash ^= d.attrsUsed;
        fp.draw_hash *= 1099511628211ull;
    }
    return fp;
}

int PlumeDraw::setVertexProgram(const uint32_t *microcode, uint32_t length,
                                const uint32_t *vertexFormat)
{
    m_curVertexProgramKey = 0;
    m_curVertexInputs = 0;
    if (!microcode || length == 0)
        return 0;
    /* FNV-1a over the microcode and the vertex format (the format-aware HLSL
     * depends on both, so the cache key must too). */
    uint64_t key = 1469598103934665603ull;
    for (uint32_t i = 0; i < length * 4u; i++) {
        key ^= microcode[i];
        key *= 1099511628211ull;
    }
    if (vertexFormat) {
        for (uint32_t i = 0; i < 16u; i++) {
            key ^= vertexFormat[i];
            key *= 1099511628211ull;
        }
    }
    auto it = m_guestVertexPrograms.find(key);
    if (it != m_guestVertexPrograms.end()) {
        GuestVertexProgram &guestProgram = it->second;
        m_curVertexInputs = guestProgram.inputsRead;
        if (!guestProgram.ok &&
            shaderCompileReady(guestProgram.compileFuture)) {
            ShaderCompileResult compiled =
                guestProgram.compileFuture.get();
            if (compiled.ok) {
                VertexProgramCommand command;
                command.key = key;
                command.inputsRead = guestProgram.inputsRead;
                command.target = compiled.target;
                command.entryPoint = std::move(compiled.entryPoint);
                command.bytecode = std::move(compiled.bytecode);
                m_rec.vertexPrograms.push_back(std::move(command));
                guestProgram.ok = true;
            } else {
                std::fprintf(stderr,
                             "[PLUME] shader compile failed: %s\n",
                             compiled.diagnostics.c_str());
            }
        }
        if (guestProgram.ok)
            m_curVertexProgramKey = key;
        return guestProgram.ok ? 1 : 0;
    }

    GuestVertexProgram guestProgram;
    static NV2AVshProgram prog;   /* large; avoid a big stack frame */
    d3d8_vsh_parse((const DWORD *)microcode, (int)length, &prog);
    guestProgram.inputsRead = prog.inputs_read;
    m_curVertexInputs = prog.inputs_read;
    static char hlsl[96 * 1024];
    int n = d3d8_vsh_generate_hlsl(&prog, vertexFormat, hlsl, (int)sizeof(hlsl));
    if (n > 0) {
        guestProgram.compileFuture = queueShaderCompile(
            m_recordShaderTarget, std::string(hlsl, static_cast<size_t>(n)),
            "vs_6_0");
    }
    m_guestVertexPrograms.emplace(key, std::move(guestProgram));
    return 0;
}

void PlumeDraw::consumeVertexPrograms(PlumeContext &ctx,
                                      FrameRecording &recording)
{
    for (VertexProgramCommand &command : recording.vertexPrograms) {
        if (m_vpCache.find(command.key) != m_vpCache.end())
            continue;
        PlumeVertexProgram program;
        program.compiled = true;
        program.inputsRead = command.inputsRead;
        program.target = command.target;
        program.entryPoint = std::move(command.entryPoint);
        const RenderShaderFormat format =
            renderShaderFormatForTarget(program.target);
        program.shader = ctx.device()->createShader(
            command.bytecode.data(), command.bytecode.size(),
            program.entryPoint.c_str(), format);
        program.ok = program.shader != nullptr;
        program.bytecode = std::move(command.bytecode);
        m_vpCache.emplace(command.key, std::move(program));
    }
    recording.vertexPrograms.clear();
}

uint32_t PlumeDraw::createPixelShader(const char *text,
                                      uint32_t cubeTextureMask)
{
    static const char kDirectHlslPrefix[] = "// XRECOMP_HLSL\n";
    if (!text || !*text) return 0;
    std::string key(text);
    auto dup = m_psByText.find(key);
    if (dup != m_psByText.end()) return dup->second;

    XpsTranslateResult tr;
    if (std::strncmp(text, kDirectHlslPrefix,
                     sizeof(kDirectHlslPrefix) - 1) == 0) {
        tr.ok = true;
        tr.hlsl = text + sizeof(kDirectHlslPrefix) - 1;
    } else {
        tr = xrecomp_xps_to_hlsl(text);
    }
    PlumePixelShader ps;
    ps.hlsl = tr.hlsl;
    ps.combinerCB =
        tr.hlsl.find("cbuffer CombinerConsts") != std::string::npos ||
        tr.hlsl.find("cbuffer CombinerCB") != std::string::npos;
    ps.cubeTextureMask = (uint8_t)(cubeTextureMask & 0xFu);
    ps.ok = !tr.hlsl.empty();
    if (ps.ok && m_pipelinesReady) {
        ps.compileFuture = queueShaderCompile(
            m_recordShaderTarget, ps.hlsl, "ps_6_0");
    }

    uint32_t h = m_psNext++;
    (void)seed_live_pixel_shader(
        h, ps.hlsl, ps.livePath, ps.liveWriteStamp);
    m_psReg.emplace(h, std::move(ps));
    m_psByText.emplace(std::move(key), h);
    return h;
}

void PlumeDraw::setActivePS(uint32_t handle) { m_activePS = handle; }

void PlumeDraw::setCombinerConsts(const float *values)
{
    setCombinerConstsEx(values, 16);
}

void PlumeDraw::setCombinerConstsEx(const float *values,
                                    uint32_t float4Count)
{
    static const float zeroConstants[21][4] = {{0}};
    const size_t totalBytes = sizeof(m_combinerConst);
    size_t copyBytes;
    if (!values)
        return;
    if (float4Count > 21)
        float4Count = 21;
    copyBytes = static_cast<size_t>(float4Count) * 4u * sizeof(float);
    if (std::memcmp(m_combinerConst, values, copyBytes) == 0 &&
        std::memcmp(
            reinterpret_cast<const uint8_t *>(m_combinerConst) + copyBytes,
            reinterpret_cast<const uint8_t *>(zeroConstants) + copyBytes,
            totalBytes - copyBytes) == 0)
        return;
    std::memcpy(m_combinerConst, values, copyBytes);
    std::memset(
        reinterpret_cast<uint8_t *>(m_combinerConst) + copyBytes, 0,
        totalBytes - copyBytes);
    ++m_combinerConstVersion;
}

bool PlumeDraw::registerVertexShader(
    uint32_t handle, const char *hlsl, uint16_t inputsRead,
    uint16_t outputsWritten,
    const XgpuPlumeVertexDeclaration *declaration)
{
    if (!handle || !hlsl || !*hlsl || !inputsRead)
        return false;
    PlumeVertexShader shader;
    shader.hlsl = hlsl;
    shader.inputsRead = inputsRead;
    shader.outputsWritten = outputsWritten;
    if (declaration) {
        size_t i;
        shader.attributesPresent = declaration->attributes_present;
        for (i = 0; i < XGPU_PLUME_VERTEX_ATTRIBUTE_COUNT; ++i) {
            shader.stream[i] = declaration->stream[i];
            shader.format[i] = declaration->format[i];
            shader.offset[i] = declaration->offset[i];
        }
        shader.hasDeclaration = true;
    }
    shader.ok = true;
    shader.compileFuture = queueShaderCompile(
        m_recordShaderTarget, shader.hlsl, "vs_6_0");
    auto existing = m_vsReg.find(handle);
    if (existing != m_vsReg.end()) {
        if (existing->second.shader)
            m_liveRetiredShaders.push_back(
                std::move(existing->second.shader));
        for (auto &pipeline : m_progPsos)
            m_liveRetiredPipelines.push_back(std::move(pipeline.second));
        m_progPsos.clear();
    }
    m_vsReg[handle] = std::move(shader);
    return true;
}

void PlumeDraw::setActiveVertexShader(uint32_t handle)
{
    m_activeVS = handle;
}

void PlumeDraw::setVertexShaderConstants(const float *values,
                                         uint32_t float4Count)
{
    if (!values)
        return;
    if (float4Count > 192)
        float4Count = 192;
    std::memset(m_vsConst, 0, sizeof(m_vsConst));
    std::memcpy(m_vsConst, values,
                static_cast<size_t>(float4Count) * 4u * sizeof(float));
    ++m_vsConstVersion;
}

void PlumeDraw::setVertexData4f(uint32_t reg, const float *value)
{
    if (!value || reg >= 16)
        return;
    std::memcpy(m_vertexData[reg], value, sizeof(m_vertexData[reg]));
}

void PlumeDraw::setPSConst(uint32_t start, const float *values, uint32_t count)
{
    uint32_t effectiveCount;
    size_t bytes;
    if (!values || count == 0)
        return;
    if (start >= 8u)
        return;
    effectiveCount = std::min<uint32_t>(count, 8u - start);
    bytes = static_cast<size_t>(effectiveCount) * 4u * sizeof(float);
    if (std::memcmp(m_psConst[start], values, bytes) == 0)
        return;
    std::memcpy(m_psConst[start], values, bytes);
    ++m_psConstVersion;
}

PlumeDraw::FrameRecording PlumeDraw::takeRecording()
{
    /* A mid-span flush (WAIT_FOR_IDLE during guest texture churn, or the
     * frame boundary) consumes the pools an open span's bases index into.
     * Slice the span's content so far into the partial packet, then re-arm
     * the bases at zero so the span survives the flush. */
    if (m_recordSpan.active)
        spanSliceInto(m_spanPartial);
    FrameRecording out = std::move(m_rec);
    out.presentTarget = m_presentTarget;
    auto presentGeneration = m_guestLatestSurfaceGeneration.find(
        out.presentTarget);
    out.presentGeneration = presentGeneration !=
            m_guestLatestSurfaceGeneration.end()
        ? presentGeneration->second : 0;
    /* Moved-from containers are valid but otherwise unspecified. */
    m_rec.clear();
    ++m_frameIndex;
    if ((m_frameIndex % 300u) == 0u &&
        (m_cachedHits | m_cachedMisses | m_cachedFallbacks) != 0u) {
        std::fprintf(stderr,
                     "[PLUME-MESH] frames=%llu hits=%u misses=%u "
                     "fallbacks=%u bytes_saved=%llu\n",
                     (unsigned long long)m_frameIndex, m_cachedHits,
                     m_cachedMisses, m_cachedFallbacks,
                     (unsigned long long)m_cachedBytesSaved);
    }
    if (m_recordSpan.active) {
        m_recordSpan.drawBase = 0;
        m_recordSpan.progBase = 0;
        m_recordSpan.vertBase = 0;
        m_recordSpan.indexBase = 0;
    }
    return out;
}

void PlumeDraw::spanBegin(uint32_t key)
{
    m_recordSpan.active = true;
    m_recordSpan.key = key;
    m_recordSpan.drawBase = m_rec.draws.size();
    m_recordSpan.progBase = m_rec.progDraws.size();
    m_recordSpan.vertBase = m_rec.frameVerts.size();
    m_recordSpan.indexBase = m_rec.frameIndices.size();
    m_spanPartial = PlumeSpanPacket();
    m_spanPartial.drawStride = static_cast<uint32_t>(sizeof(GeomDraw));
    m_spanRejected = false;
}

/* Append the open span's content since its current bases into `packet`,
 * rebasing offsets to packet-local space. Called by spanEnd and, for a
 * span crossing a flush, by takeRecording before the pools are moved. */
void PlumeDraw::spanSliceInto(PlumeSpanPacket &packet)
{
    static_assert(std::is_trivially_copyable<GeomDraw>::value,
                  "span packets memcpy GeomDraw blobs");

    if (m_spanRejected)
        return;
    /* Defensive: never slice with bases beyond the live pools. */
    if (m_recordSpan.drawBase > m_rec.draws.size() ||
        m_recordSpan.progBase > m_rec.progDraws.size() ||
        m_recordSpan.vertBase > m_rec.frameVerts.size() ||
        m_recordSpan.indexBase > m_rec.frameIndices.size()) {
        m_spanRejected = true;
        return;
    }
    /* ProgDraws interleaved inside the span are not representable in a
     * v1 packet; fail closed to guest drawing for this key. */
    if (m_rec.progDraws.size() != m_recordSpan.progBase) {
        m_spanRejected = true;
        return;
    }
    const size_t drawEnd = m_rec.draws.size();
    if (drawEnd == m_recordSpan.drawBase)
        return;

    const size_t vertBase = m_recordSpan.vertBase;
    const size_t indexBase = m_recordSpan.indexBase;
    const size_t packetVertBase = packet.vertexBytes.size();
    const size_t packetIndexBase = packet.indexData.size();
    /* In-span ranges are one contiguous tail of each frame pool. */
    packet.vertexBytes.insert(packet.vertexBytes.end(),
                              m_rec.frameVerts.begin() + vertBase,
                              m_rec.frameVerts.end());
    packet.indexData.insert(packet.indexData.end(),
                            m_rec.frameIndices.begin() + indexBase,
                            m_rec.frameIndices.end());

    /* Frame-pool windows are already deduplicated; keep that sharing so
     * injection re-interns each unique window once, not once per draw.
     * Pool indices are only stable within this segment. */
    std::unordered_map<uint32_t, uint32_t> vsDedup;
    std::unordered_map<uint32_t, uint32_t> psDedup;

    for (size_t di = m_recordSpan.drawBase; di < drawEnd; ++di) {
        const GeomDraw &g = m_rec.draws[di];
        /* Clears, blits, and present-relative draws must never replay. */
        if (g.clear || g.clearDepth || g.clearStencil || g.hasClearRect ||
            g.blitSrcGeneration || g.blitDstGeneration ||
            g.afterPresentCopy) {
            m_spanRejected = true;
            return;
        }

        PlumeSpanDrawRef ref;
        if (g.cachedMeshId) {
            ref.vertOffsetInPacket = 0;
            ref.vertByteLen = 0;
            ref.indexOffsetInPacket = 0;
            ref.indexCount = 0;
            if (g.vsHandle && g.vsConstIndex < m_rec.frameVSConsts.size()) {
                auto it = vsDedup.find(g.vsConstIndex);
                if (it != vsDedup.end()) {
                    ref.vsWindowIndex = it->second;
                } else {
                    ref.vsWindowIndex =
                        static_cast<uint32_t>(packet.vsWindows.size());
                    packet.vsWindows.push_back(
                        m_rec.frameVSConsts[g.vsConstIndex]);
                    vsDedup.emplace(g.vsConstIndex, ref.vsWindowIndex);
                }
            }
            if (g.progConstIndex < m_rec.frameProgConsts.size()) {
                auto it = psDedup.find(g.progConstIndex);
                if (it != psDedup.end()) {
                    ref.psWindowIndex = it->second;
                } else {
                    const std::array<float, kProgramConstantFloatCount> &ps =
                        m_rec.frameProgConsts[g.progConstIndex];
                    ref.psWindowIndex =
                        static_cast<uint32_t>(packet.psWindows.size());
                    packet.psWindows.emplace_back(ps.begin(), ps.end());
                    psDedup.emplace(g.progConstIndex, ref.psWindowIndex);
                }
            }
            const size_t blobAt = packet.geomBlobs.size();
            packet.geomBlobs.resize(blobAt + sizeof(GeomDraw));
            std::memcpy(packet.geomBlobs.data() + blobAt, &g, sizeof(GeomDraw));
            packet.draws.push_back(ref);
            continue;
        }
        if (g.offset >= vertBase) {
            ref.vertOffsetInPacket = static_cast<uint32_t>(
                packetVertBase + (g.offset - vertBase));
        } else {
            /* The recorder dedups identical byte runs, so a draw may
             * reference vertex bytes appended before the span. Append a
             * private copy after the bulk slice. */
            ref.vertOffsetInPacket =
                static_cast<uint32_t>(packet.vertexBytes.size());
            packet.vertexBytes.insert(packet.vertexBytes.end(),
                                      m_rec.frameVerts.begin() + g.offset,
                                      m_rec.frameVerts.begin() + g.offset
                                          + g.byteLen);
        }
        ref.vertByteLen = g.byteLen;
        ref.indexCount = g.indexCount;
        if (g.indexCount) {
            if (g.indexOffset >= indexBase) {
                ref.indexOffsetInPacket = static_cast<uint32_t>(
                    packetIndexBase + (g.indexOffset - indexBase));
            } else {
                ref.indexOffsetInPacket =
                    static_cast<uint32_t>(packet.indexData.size());
                packet.indexData.insert(
                    packet.indexData.end(),
                    m_rec.frameIndices.begin() + g.indexOffset,
                    m_rec.frameIndices.begin() + g.indexOffset
                        + g.indexCount);
            }
        }

        if (g.vsHandle && g.vsConstIndex < m_rec.frameVSConsts.size()) {
            auto it = vsDedup.find(g.vsConstIndex);
            if (it != vsDedup.end()) {
                ref.vsWindowIndex = it->second;
            } else {
                ref.vsWindowIndex =
                    static_cast<uint32_t>(packet.vsWindows.size());
                packet.vsWindows.push_back(
                    m_rec.frameVSConsts[g.vsConstIndex]);
                vsDedup.emplace(g.vsConstIndex, ref.vsWindowIndex);
            }
        }
        if (g.progConstIndex < m_rec.frameProgConsts.size()) {
            auto it = psDedup.find(g.progConstIndex);
            if (it != psDedup.end()) {
                ref.psWindowIndex = it->second;
            } else {
                const std::array<float, kProgramConstantFloatCount> &ps =
                    m_rec.frameProgConsts[g.progConstIndex];
                ref.psWindowIndex =
                    static_cast<uint32_t>(packet.psWindows.size());
                packet.psWindows.emplace_back(ps.begin(), ps.end());
                psDedup.emplace(g.progConstIndex, ref.psWindowIndex);
            }
        }

        const size_t blobAt = packet.geomBlobs.size();
        packet.geomBlobs.resize(blobAt + sizeof(GeomDraw));
        std::memcpy(packet.geomBlobs.data() + blobAt, &g, sizeof(GeomDraw));
        packet.draws.push_back(ref);
    }
}

uint32_t PlumeDraw::spanEnd(uint32_t key)
{
    if (!m_recordSpan.active || m_recordSpan.key != key) {
        m_recordSpan.active = false;
        return 0;
    }
    m_recordSpan.active = false;
    spanSliceInto(m_spanPartial);

    const uint32_t total =
        static_cast<uint32_t>(m_spanPartial.draws.size());
    if (!m_spanRejected && total != 0) {
        m_spanPartial.frameStamp = m_frameIndex;
        m_spanReplay.store(key, std::move(m_spanPartial));
    }
    m_spanPartial = PlumeSpanPacket();
    return total;
}

bool PlumeDraw::spanTryReplay(uint32_t key)
{
    /* Target donor: the last GeomDraw the guest recorded this frame (clears
     * included — their render-target identity/generations are current). By
     * value: appends below may reallocate draws. */
    if (m_rec.draws.empty())
        return false;
    const GeomDraw donor = m_rec.draws.back();

    const PlumeSpanPacket *packet =
        m_spanReplay.find(key, m_frameIndex, kSpanReplayMaxAgeFrames);
    if (!packet)
        return false;

    if (packet->drawStride != sizeof(GeomDraw))
        return false;

    /* Texture staleness: every referenced texture must still be the current
     * generation, otherwise drop the packet and let the guest re-record. */
    for (size_t di = 0; di < packet->draws.size(); ++di) {
        GeomDraw g;
        std::memcpy(&g, packet->geomBlobs.data() + di * sizeof(GeomDraw),
                    sizeof(GeomDraw));
        for (uint8_t t = 0; t < g.texCount && t < 4; ++t) {
            const RecordedTextureBinding &binding = g.stageTexture[t];
            if (!binding.valid())
                continue;
            const RecordedTextureVersions<PlumeTex>::Entry *entry =
                m_textures.current(binding.guest);
            if (!entry || !entry->binding.sameResource(binding)) {
                m_spanReplay.erase(key);
                return false;
            }
        }
    }

    /* Camera donor: the most recent draw this frame carrying an interned VS
     * window (the frame's opening draws are often clears, which have none).
     * Fall back to the live register file, which the guest refreshes during
     * frame setup before any city drawing. Copy by value: interning below
     * may reallocate frameVSConsts. */
    float cameraRows[16];
    bool haveCamera = false;
    size_t scanAt = m_rec.draws.size();
    for (size_t scanned = 0; scanned < 64 && scanAt > 0; ++scanned) {
        const GeomDraw &d = m_rec.draws[--scanAt];
        if (d.vsHandle && d.vsConstIndex < m_rec.frameVSConsts.size()) {
            std::memcpy(cameraRows,
                        m_rec.frameVSConsts[d.vsConstIndex].data()
                            + 96u * 4u,
                        sizeof(cameraRows));
            haveCamera = true;
            break;
        }
    }
    if (!haveCamera)
        std::memcpy(cameraRows, &m_vsConst[96][0], sizeof(cameraRows));

    /* Re-intern each unique window exactly once (windows were deduplicated
     * at slice time); draws then map straight to frame-pool indices. */
    std::vector<uint32_t> vsMap(packet->vsWindows.size());
    for (size_t wi = 0; wi < packet->vsWindows.size(); ++wi) {
        std::array<float, 768> window = packet->vsWindows[wi];
        plume_span_replay_patch_camera(window, cameraRows);
        vsMap[wi] = internFrameConstants(
            m_rec.frameVSConsts, m_rec.frameVSConstBuckets, window);
    }
    std::vector<uint32_t> psMap(packet->psWindows.size());
    for (size_t wi = 0; wi < packet->psWindows.size(); ++wi) {
        std::array<float, kProgramConstantFloatCount> ps{};
        std::memcpy(ps.data(), packet->psWindows[wi].data(),
                    ps.size() * sizeof(float));
        psMap[wi] = internFrameConstants(
            m_rec.frameProgConsts, m_rec.frameProgConstBuckets, ps);
    }

    const size_t vertBase = m_rec.frameVerts.size();
    m_rec.frameVerts.insert(m_rec.frameVerts.end(),
                            packet->vertexBytes.begin(),
                            packet->vertexBytes.end());
    const size_t indexBase = m_rec.frameIndices.size();
    m_rec.frameIndices.insert(m_rec.frameIndices.end(),
                              packet->indexData.begin(),
                              packet->indexData.end());

    m_rec.draws.reserve(m_rec.draws.size() + packet->draws.size());
    for (size_t di = 0; di < packet->draws.size(); ++di) {
        const PlumeSpanDrawRef &ref = packet->draws[di];
        GeomDraw g;
        std::memcpy(&g, packet->geomBlobs.data() + di * sizeof(GeomDraw),
                    sizeof(GeomDraw));

        if (!g.cachedMeshId) {
            g.offset = static_cast<uint32_t>(vertBase + ref.vertOffsetInPacket);
            if (ref.indexCount)
                g.indexOffset = static_cast<uint32_t>(
                    indexBase + ref.indexOffsetInPacket);
        }

        if (g.vsHandle && ref.vsWindowIndex != UINT32_MAX)
            g.vsConstIndex = vsMap[ref.vsWindowIndex];
        if (ref.psWindowIndex != UINT32_MAX)
            g.progConstIndex = psMap[ref.psWindowIndex];

        /* Current-frame surface identity from the donor draw. */
        g.targetGuest = donor.targetGuest;
        g.targetWidth = donor.targetWidth;
        g.targetHeight = donor.targetHeight;
        g.targetColorGeneration = donor.targetColorGeneration;
        g.targetZetaGeneration = donor.targetZetaGeneration;
        g.targetFramebufferGeneration = donor.targetFramebufferGeneration;
        std::memcpy(g.surfaceStage, donor.surfaceStage,
                    sizeof(g.surfaceStage));

        m_rec.draws.push_back(g);
    }
    return true;
}

void PlumeDraw::spanInvalidate(uint32_t key)
{
    m_spanReplay.erase(key);
}

void PlumeDraw::spanInvalidateMask(uint32_t keyMask, uint32_t keyBits)
{
    m_spanReplay.invalidateMask(keyMask, keyBits);
}

void PlumeDraw::spanInvalidateAll()
{
    m_spanReplay.invalidateAll();
}

void PlumeDraw::replay(PlumeContext &ctx, ::plume::RenderCommandList *cl,
                       ::plume::RenderTexture *screenTexture,
                       ::plume::RenderFramebuffer *screenFramebuffer,
                       bool copyPresentSurface, bool frameBoundary)
{
    FrameRecording recording = takeRecording();
    replayRecording(ctx, std::move(recording), cl, screenTexture,
                    screenFramebuffer, copyPresentSurface, frameBoundary);
    /* The synchronous wrapper can recycle the emptied vector capacities.
     * Task 4's cross-thread caller will instead return recordings through its
     * bounded ownership queue. */
    std::swap(m_rec, recording);
}

void PlumeDraw::replayRecording(
    PlumeContext &ctx, FrameRecording &&rec, ::plume::RenderCommandList *cl,
    ::plume::RenderTexture *screenTexture,
    ::plume::RenderFramebuffer *screenFramebuffer,
    bool copyPresentSurface, bool frameBoundary)
{
    const uint32_t presentTarget = rec.presentTarget;
    const uint64_t presentGeneration = rec.presentGeneration;
    /* Materialize logical surface generations before write publication or
     * draw replay, then publish texture payloads before resolving bindings. */
    consumeSurfaceBindings(ctx, rec);
    consumeTextureUploads(ctx, rec, cl);
    consumeVertexPrograms(ctx, rec);
    /* Surface cache serials are render-owned. Apply the guest's recorded
     * write intent here before any early replay rejection, preserving the
     * conservative CPU-read behavior without cross-thread cache mutation. */
    for (const GeomDraw &draw : rec.draws) {
        if (draw.recordsColorWrite)
            noteColorWrite(draw.targetColorGeneration);
        if (draw.recordsZetaWrite)
            noteZetaWrite(draw.targetZetaGeneration);
    }
    for (const ProgDraw &draw : rec.progDraws) {
        if (draw.recordsColorWrite)
            noteColorWrite(draw.targetColorGeneration);
        if (draw.recordsZetaWrite)
            noteZetaWrite(draw.targetZetaGeneration);
    }
    XRECOMP_TRACY_ZONE_SCOPED("Plume Replay");
    XRECOMP_CPU_RECORDER_ZONE_SCOPED("Plume Replay");
    auto clearFrame = [&]() {
        m_frameDrawCounter.recordSubmission(
            rec.draws.size() + rec.progDraws.size(), frameBoundary);
        rec.clear();
    };
    if (!m_geomReady || !cl || !screenTexture || !screenFramebuffer) {
        clearFrame();
        return;
    }
    pollPixelShaderOverrides(ctx);

    /* Per-submission upload buffers: this batch may be submitted without a fence
     * (async WAIT_FOR_IDLE ring), so its vertex/const data must live in a buffer
     * no concurrently-recording batch will overwrite. */
    Submission &sub = m_sub[m_curSub];

    /* F2 capture (plume_f2_capture.h): when armed, log this replay's full
     * draw stream — including silently skipped draws — with host resource
     * identity. Zero work when no capture is active. */
    const bool f2On = xgpu_plume_f2_active() != 0;
    if (f2On) {
        xgpu_plume_f2_log("replay sub=%u boundary=%u geom=%zu prog=%zu "
                          "present=%08X copy=%u",
                          m_curSub, frameBoundary ? 1u : 0u, rec.draws.size(),
                           rec.progDraws.size(), presentTarget,
                          copyPresentSurface ? 1u : 0u);
        /* Combiner-constant contents per interned slot (kc[0..15], fc0/fc1,
         * misc): the cc=slot:hash draw fields refer to these. */
        for (size_t ci = 0; ci < rec.frameProgConsts.size(); ci++) {
            const std::array<float, kProgramConstantFloatCount> &cv =
                rec.frameProgConsts[ci];
            char line[1600];
            size_t off = 0;
            for (size_t k = 0; k < cv.size() && off + 16 < sizeof line; k++) {
                int n = std::snprintf(line + off, sizeof line - off,
                                      "%s%g", k ? "," : "", cv[k]);
                if (n < 0)
                    break;
                off += (size_t)n;
            }
            line[off < sizeof line ? off : sizeof line - 1] = '\0';
            xgpu_plume_f2_log("ccval %zu %s", ci, line);
        }
        /* Programmable draws refer to these by vscc=slot:hash. Emit only
         * nonzero float4 registers so screen-space transforms remain readable
         * without turning a three-frame capture into 192 lines per slot. */
        for (size_t vi = 0; vi < rec.frameVSConsts.size(); ++vi) {
            const auto &constants = rec.frameVSConsts[vi];
            for (uint32_t reg = 0; reg < 192u; ++reg) {
                const float *value = constants.data() + reg * 4u;
                uint32_t bits[4];
                std::memcpy(bits, value, sizeof(bits));
                if (!(bits[0] | bits[1] | bits[2] | bits[3]))
                    continue;
                xgpu_plume_f2_log(
                    "vsval %zu c%u=(%.9g,%.9g,%.9g,%.9g)",
                    vi, reg, value[0], value[1], value[2], value[3]);
            }
        }
    }
    auto f2Hash = [](const void *data, size_t bytes) -> unsigned long long {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        unsigned long long h = 1469598103934665603ull;
        for (size_t i = 0; i < bytes; i++)
            h = (h ^ p[i]) * 1099511628211ull;
        return h;
    };
    /* Stage identity comes from immutable recording keys, never cache/RHI
     * pointers, so diagnostics remain valid after a same-address replacement. */
    auto f2Stages = [&](char *out, size_t cap, const uint64_t *surf,
                         const RecordedTextureBinding *tex) {
        size_t used = 0;
        for (uint32_t s = 0; s < 4; s++) {
            int n = 0;
            if (surf[s])
                n = std::snprintf(out + used, cap - used, "%sS%llu",
                                  s ? "," : "", (unsigned long long)surf[s]);
            else if (tex[s].valid()) {
                n = std::snprintf(out + used, cap - used,
                                  "%sg%08X:%ux%u/f%02X/u%u:v%016llX",
                                  s ? "," : "", tex[s].guest,
                                  tex[s].width, tex[s].height, tex[s].format,
                                  tex[s].unnormalizedCoords,
                                  (unsigned long long)tex[s].version);
            } else
                n = std::snprintf(out + used, cap - used, "%s-",
                                  s ? "," : "");
            if (n < 0 || (size_t)n >= cap - used) {
                used = cap - 1;
                break;
            }
            used += (size_t)n;
        }
        out[used] = '\0';
    };
    auto f2Samplers = [&](char *out, size_t cap,
                          const XgpuSamplerBinding *sampler,
                          const uint8_t *valid) {
        size_t used = 0;
        for (uint32_t s = 0; s < 4; ++s) {
            int n;
            if (!valid[s]) {
                n = std::snprintf(out + used, cap - used, "%s-",
                                  s ? "," : "");
            } else {
                n = std::snprintf(
                    out + used, cap - used,
                    "%sa%u%u%u/f%u%u%u/b%.4g/m%u/x%u",
                    s ? "," : "", sampler[s].address_u,
                    sampler[s].address_v, sampler[s].address_w,
                    sampler[s].min_filter, sampler[s].mag_filter,
                    sampler[s].mip_filter, sampler[s].mip_lod_bias,
                    sampler[s].max_mip_level, sampler[s].max_anisotropy);
            }
            if (n < 0 || static_cast<size_t>(n) >= cap - used) {
                used = cap - 1;
                break;
            }
            used += static_cast<size_t>(n);
        }
        out[used] = '\0';
    };

    /* Spike diagnosis: section accumulators reported when one replay stalls. */
    const uint64_t spikeT0 = xgpu_plume_replay_spike_enabled()
        ? xrecomp_host_monotonic_ns() : 0;
    uint64_t nsUpload = 0, nsDesc = 0, nsFlush = 0, nsPresent = 0;
    uint32_t flushCount = 0, descCreates = 0;
    /* Largest single gap between marks: distinguishes one blocking call
     * (one giant labeled gap) from thread preemption (random placement). */
    uint64_t lastMark = spikeT0, maxGap = 0;
    const char *maxGapLabel = "";
    auto mark = [&](const char *label) {
        if (!spikeT0)
            return;
        const uint64_t now = xrecomp_host_monotonic_ns();
        if (now - lastMark > maxGap) {
            maxGap = now - lastMark;
            maxGapLabel = label;
        }
        lastMark = now;
    };

    XRECOMP_TRACY_ZONE_BEGIN(replay_upload_zone, "Plume Replay Upload");
    const size_t need = rec.frameVerts.size();
    if (need) {
        XRECOMP_TRACY_ZONE_BEGIN(vertex_upload_zone,
                                "Plume Upload Vertex Stream");
        if (need > sub.vbufCap) {
            size_t cap = 1u << 16;
            while (cap < need) cap <<= 1;
            {
                XRECOMP_TRACY_ZONE_SCOPED("Plume Create Upload Buffer");
                sub.vbuf = ctx.device()->createBuffer(
                    ::plume::RenderBufferDesc::VertexBuffer(
                        cap, ::plume::RenderHeapType::UPLOAD));
            }
            sub.vbufCap = sub.vbuf ? cap : 0;
        }
        if (!sub.vbuf) {
            XRECOMP_TRACY_ZONE_END(vertex_upload_zone);
            XRECOMP_TRACY_ZONE_END(replay_upload_zone);
            clearFrame();
            return;
        }
        if (!copyToMappedBuffer(sub.vbuf.get(), rec.frameVerts.data(), need)) {
            std::fprintf(stderr,
                         "[PLUME] upload map failed: vertices bytes=%zu "
                         "slot=%u\n",
                         need, m_curSub);
            XRECOMP_TRACY_ZONE_END(vertex_upload_zone);
            XRECOMP_TRACY_ZONE_END(replay_upload_zone);
            clearFrame();
            return;
        }
        XRECOMP_TRACY_ZONE_VALUE(vertex_upload_zone, need);
        XRECOMP_TRACY_ZONE_END(vertex_upload_zone);
    }

    if (!rec.frameProgConsts.empty()) {
        XRECOMP_TRACY_ZONE_BEGIN(pixel_constants_upload_zone,
                                "Plume Upload Pixel Constants");
        const size_t constantsNeed =
            rec.frameProgConsts.size() * kProgramConstantStride;
        if (constantsNeed > sub.progCBCap) {
            size_t cap = kProgramConstantStride;
            while (cap < constantsNeed) cap <<= 1;
            {
                XRECOMP_TRACY_ZONE_SCOPED("Plume Create Upload Buffer");
                sub.progCB = ctx.device()->createBuffer(
                    RenderBufferDesc::UploadBuffer(
                        cap, RenderBufferFlag::CONSTANT));
            }
            sub.progCBCap = sub.progCB ? cap : 0;
        }
        if (sub.progCB) {
            uint8_t *constants = nullptr;
            {
                XRECOMP_TRACY_ZONE_SCOPED("Plume Buffer Map");
                constants = static_cast<uint8_t *>(sub.progCB->map());
            }
            if (!constants) {
                std::fprintf(stderr,
                             "[PLUME] upload map failed: pixel constants "
                             "bytes=%zu slot=%u\n",
                             constantsNeed, m_curSub);
                XRECOMP_TRACY_ZONE_END(pixel_constants_upload_zone);
                XRECOMP_TRACY_ZONE_END(replay_upload_zone);
                clearFrame();
                return;
            }
            {
                XRECOMP_TRACY_ZONE_SCOPED("Plume Buffer Memcpy");
                for (size_t i = 0; i < rec.frameProgConsts.size(); i++)
                    std::memcpy(constants + i * kProgramConstantStride,
                                rec.frameProgConsts[i].data(),
                                rec.frameProgConsts[i].size() * sizeof(float));
            }
            {
                XRECOMP_TRACY_ZONE_SCOPED("Plume Buffer Unmap");
                sub.progCB->unmap();
            }
        }
        XRECOMP_TRACY_ZONE_VALUE(pixel_constants_upload_zone, constantsNeed);
        XRECOMP_TRACY_ZONE_END(pixel_constants_upload_zone);
    }

    /* Upload indexed draws' raw vertex stream (programmable NV2A path) and
     * the shared owned uint32 index stream (programmable and compatibility
     * GeomDraw paths). */
    if (!rec.frameRawVerts.empty() || !rec.frameIndices.empty()) {
        XRECOMP_TRACY_ZONE_BEGIN(indexed_streams_upload_zone,
                                "Plume Upload Indexed Streams");
        const size_t rvNeed = rec.frameRawVerts.size();
        if (rvNeed && rvNeed > sub.rawVbufCap) {
            size_t cap = 1u << 16;
            while (cap < rvNeed) cap <<= 1;
            {
                XRECOMP_TRACY_ZONE_SCOPED("Plume Create Upload Buffer");
                sub.rawVbuf = ctx.device()->createBuffer(
                    RenderBufferDesc::VertexBuffer(
                        cap, RenderHeapType::UPLOAD));
            }
            sub.rawVbufCap = sub.rawVbuf ? cap : 0;
        }
        const size_t ibNeed = rec.frameIndices.size() * sizeof(uint32_t);
        if (ibNeed > sub.ibufCap) {
            size_t cap = 1u << 12;
            while (cap < ibNeed) cap <<= 1;
            {
                XRECOMP_TRACY_ZONE_SCOPED("Plume Create Upload Buffer");
                sub.ibuf = ctx.device()->createBuffer(
                    RenderBufferDesc::IndexBuffer(
                        cap, RenderHeapType::UPLOAD));
            }
            sub.ibufCap = sub.ibuf ? cap : 0;
        }
        if (rvNeed && sub.rawVbuf) {
            if (!copyToMappedBuffer(
                    sub.rawVbuf.get(), rec.frameRawVerts.data(), rvNeed)) {
                std::fprintf(stderr,
                             "[PLUME] upload map failed: raw vertices "
                             "bytes=%zu slot=%u\n",
                             rvNeed, m_curSub);
                XRECOMP_TRACY_ZONE_END(indexed_streams_upload_zone);
                XRECOMP_TRACY_ZONE_END(replay_upload_zone);
                clearFrame();
                return;
            }
        }
        if (ibNeed && sub.ibuf) {
            if (!copyToMappedBuffer(
                    sub.ibuf.get(), rec.frameIndices.data(), ibNeed)) {
                std::fprintf(stderr,
                             "[PLUME] upload map failed: indices bytes=%zu "
                             "slot=%u\n",
                             ibNeed, m_curSub);
                XRECOMP_TRACY_ZONE_END(indexed_streams_upload_zone);
                XRECOMP_TRACY_ZONE_END(replay_upload_zone);
                clearFrame();
                return;
            }
        }
        XRECOMP_TRACY_ZONE_VALUE(indexed_streams_upload_zone,
                                 rvNeed + ibNeed);
        XRECOMP_TRACY_ZONE_END(indexed_streams_upload_zone);
    }

    /* Both CPU-decoded and indexed programmable draws share one portable
     * b9 constant-buffer upload, addressed in 192-float4 slots. */
    if (!rec.frameVSConsts.empty()) {
        XRECOMP_TRACY_ZONE_BEGIN(vertex_constants_upload_zone,
                                "Plume Upload Vertex Constants");
        const size_t constantsNeed = rec.frameVSConsts.size() * 3072u;
        if (constantsNeed > sub.vsCBCap) {
            size_t cap = 4096u;
            while (cap < constantsNeed) cap <<= 1;
            {
                XRECOMP_TRACY_ZONE_SCOPED("Plume Create Upload Buffer");
                sub.vsCB = ctx.device()->createBuffer(
                    RenderBufferDesc::UploadBuffer(
                        cap, RenderBufferFlag::CONSTANT));
            }
            sub.vsCBCap = sub.vsCB ? cap : 0;
        }
        if (sub.vsCB) {
            uint8_t *constants = nullptr;
            {
                XRECOMP_TRACY_ZONE_SCOPED("Plume Buffer Map");
                constants = static_cast<uint8_t *>(sub.vsCB->map());
            }
            if (!constants) {
                std::fprintf(stderr,
                             "[PLUME] upload map failed: vertex constants "
                             "bytes=%zu slot=%u\n",
                             constantsNeed, m_curSub);
                XRECOMP_TRACY_ZONE_END(vertex_constants_upload_zone);
                XRECOMP_TRACY_ZONE_END(replay_upload_zone);
                clearFrame();
                return;
            }
            {
                XRECOMP_TRACY_ZONE_SCOPED("Plume Buffer Memcpy");
                for (size_t i = 0; i < rec.frameVSConsts.size(); ++i)
                    std::memcpy(constants + i * 3072u,
                                rec.frameVSConsts[i].data(), 3072u);
            }
            {
                XRECOMP_TRACY_ZONE_SCOPED("Plume Buffer Unmap");
                sub.vsCB->unmap();
            }
        }
        XRECOMP_TRACY_ZONE_VALUE(vertex_constants_upload_zone, constantsNeed);
        XRECOMP_TRACY_ZONE_END(vertex_constants_upload_zone);
    }
    XRECOMP_TRACY_ZONE_END(replay_upload_zone);

    if (spikeT0)
        nsUpload = xrecomp_host_monotonic_ns() - spikeT0;
    mark("upload");

    RenderTextureLayout screenLayout = RenderTextureLayout::COLOR_WRITE;
    auto transitionSurface = [&](PlumeColorSurface &surface,
                                 RenderTextureLayout layout, bool snapshot) {
        RenderTextureLayout &current = snapshot ? surface.snapshotLayout
                                                : surface.layout;
        if (current == layout)
            return;
        RenderTexture *texture = snapshot ? surface.snapshot.get()
                                          : surface.texture.get();
        RenderBarrierStages stage =
            (layout == RenderTextureLayout::COPY_SOURCE ||
             layout == RenderTextureLayout::COPY_DEST)
                ? RenderBarrierStage::COPY
                : RenderBarrierStage::GRAPHICS;
        cl->barriers(stage, RenderTextureBarrier(texture, layout));
        current = layout;
        mark("barrier");
    };
    auto transitionDepth = [&](PlumeZetaSurface &surface) {
        if (surface.layout == RenderTextureLayout::DEPTH_WRITE)
            return;
        cl->barriers(RenderBarrierStage::GRAPHICS,
                     RenderTextureBarrier(surface.texture.get(),
                                          RenderTextureLayout::DEPTH_WRITE));
        surface.layout = RenderTextureLayout::DEPTH_WRITE;
        mark("barrier-z");
    };
    bool presentSurfaceCopied = false;
    auto copyPresentTarget = [&]() {
        if (presentSurfaceCopied)
            return;
        presentSurfaceCopied = true;
        XRECOMP_TRACY_ZONE_SCOPED("Plume Replay Present Copy");
        const uint64_t presentT0 = spikeT0
            ? xrecomp_host_monotonic_ns() : 0;
        if (copyPresentSurface && presentTarget) {
            auto latest = m_latestSurfaceGeneration.find(presentTarget);
            const uint64_t generation = presentGeneration
                ? presentGeneration
                : (latest != m_latestSurfaceGeneration.end()
                       ? latest->second : 0);
            auto presentIt = generation
                ? m_surfaceCache.find(generation) : m_surfaceCache.end();
            if (presentIt != m_surfaceCache.end()) {
                PlumeColorSurface &present = presentIt->second;
                if (present.width == m_outputWidth &&
                    present.height == m_outputHeight) {
                    transitionSurface(present,
                                  RenderTextureLayout::COPY_SOURCE, false);
                    cl->barriers(
                        RenderBarrierStage::COPY,
                        RenderTextureBarrier(
                            screenTexture, RenderTextureLayout::COPY_DEST));
                    screenLayout = RenderTextureLayout::COPY_DEST;
                    cl->copyTexture(screenTexture, present.texture.get());
                    if (f2On)
                        xgpu_plume_f2_log(
                            "presentcopy tgt=%08X gen=%llu %ux%u",
                            presentTarget,
                            (unsigned long long)generation,
                            present.width, present.height);
                } else {
                    transitionSurface(present,
                                      RenderTextureLayout::SHADER_READ, false);
                    if (screenLayout != RenderTextureLayout::COLOR_WRITE) {
                        cl->barriers(
                            RenderBarrierStage::GRAPHICS,
                            RenderTextureBarrier(
                                screenTexture,
                                RenderTextureLayout::COLOR_WRITE));
                        screenLayout = RenderTextureLayout::COLOR_WRITE;
                    }
                    if (recordOutputScale(
                            ctx, cl, present.texture.get(), present.view.get(),
                            screenFramebuffer, physicalOutputWidth(),
                            physicalOutputHeight()) && f2On) {
                        xgpu_plume_f2_log(
                            "presentscale tgt=%08X gen=%llu %ux%u -> %ux%u",
                            presentTarget,
                            (unsigned long long)generation,
                            present.width, present.height,
                            m_outputWidth, m_outputHeight);
                    }
                }
            } else if (f2On) {
                xgpu_plume_f2_log(
                    "presentcopy MISS tgt=%08X gen=%llu found=%d",
                    presentTarget,
                    (unsigned long long)generation,
                    presentIt != m_surfaceCache.end());
            }
        }
        if (presentT0)
            nsPresent += xrecomp_host_monotonic_ns() - presentT0;
        mark("present-copy");
    };
    auto submitDescriptorBatch = [&]() {
        XRECOMP_TRACY_ZONE_SCOPED("Plume Descriptor Batch Flush");
        const uint64_t flushT0 = spikeT0 ? xrecomp_host_monotonic_ns() : 0;
        cl->end();
        const RenderCommandList *submitted = cl;
        ctx.queue()->executeCommandLists(&submitted, 1, nullptr, 0, nullptr, 0,
                                         ctx.fence());
        uint64_t wait_t0 = xgpu_plume_wait_stats_begin();
        ctx.queue()->waitForCommandFence(ctx.fence());
        xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_DESC_FLUSH, wait_t0);
        m_progDescCache.clear();
        /* The flush wait just established total queue order, which makes this
         * the pipelined-present substitute for the synchronous present's
         * retired-texture discard. */
        if (m_pipelinedPresent)
            m_textures.discardRetiredResources();
        cl->begin();
        if (flushT0) {
            nsFlush += xrecomp_host_monotonic_ns() - flushT0;
            flushCount++;
        }
        mark("flush");
    };

    /* Render one deferred indexed programmable draw (GPU vertex transform). */
    auto renderProgDraw = [&](const ProgDraw &pd, size_t pi) {
        if (!m_progReady || !sub.rawVbuf || !sub.ibuf || !sub.vsCB ||
            !sub.progCB || pd.vsConstIndex >= rec.frameVSConsts.size()) {
            if (f2On)
                xgpu_plume_f2_log("p %zu SKIP resources", pi);
            return;
        }
        PlumeColorSurface *target = nullptr;
        PlumeZetaSurface *targetZeta = nullptr;
        RenderFramebuffer *targetFramebuffer = screenFramebuffer;
        uint32_t targetWidth = m_outputWidth;
        uint32_t targetHeight = m_outputHeight;
        if (pd.targetGuest) {
            auto targetIt = m_surfaceCache.find(pd.targetColorGeneration);
            auto fbIt = m_framebufferCache.find(pd.targetFramebufferGeneration);
            if (targetIt == m_surfaceCache.end() ||
                fbIt == m_framebufferCache.end() ||
                targetIt->second.width != pd.targetWidth ||
                targetIt->second.height != pd.targetHeight) {
                if (f2On)
                    xgpu_plume_f2_log("p %zu SKIP target tgt=%08X %ux%u "
                                      "cg=%llu fb=%llu", pi, pd.targetGuest,
                                      pd.targetWidth, pd.targetHeight,
                                      (unsigned long long)pd.targetColorGeneration,
                                      (unsigned long long)pd.targetFramebufferGeneration);
                return;
            }
            target = &targetIt->second;
            if (pd.targetZetaGeneration) {
                auto zIt = m_zetaCache.find(pd.targetZetaGeneration);
                if (zIt == m_zetaCache.end()) {
                    if (f2On)
                        xgpu_plume_f2_log("p %zu SKIP zeta tgt=%08X zg=%llu",
                                          pi, pd.targetGuest,
                                          (unsigned long long)pd.targetZetaGeneration);
                    return;
                }
                targetZeta = &zIt->second;
            }
            targetFramebuffer = fbIt->second.framebuffer.get();
            targetWidth = target->width;
            targetHeight = target->height;
        }
        uint32_t physicalTargetWidth = 0;
        uint32_t physicalTargetHeight = 0;
        if (!plumeScaledExtent(targetWidth, targetHeight,
                               m_internalResolutionScale,
                               &physicalTargetWidth,
                               &physicalTargetHeight))
            return;
        if (target) {
            if (m_replayWritten.empty() ||
                m_replayWritten.back() != pd.targetColorGeneration)
                m_replayWritten.push_back(pd.targetColorGeneration);
            transitionSurface(*target, RenderTextureLayout::COLOR_WRITE, false);
            /* The framebuffer binds its depth attachment even when this
             * particular draw disables depth testing. Backends therefore
             * require an attachment layout before setFramebuffer, not only
             * before a depth-writing draw. */
            if (targetZeta)
                transitionDepth(*targetZeta);
        } else if (screenLayout != RenderTextureLayout::COLOR_WRITE) {
            cl->barriers(RenderBarrierStage::GRAPHICS,
                         RenderTextureBarrier(screenTexture,
                                              RenderTextureLayout::COLOR_WRITE));
            screenLayout = RenderTextureLayout::COLOR_WRITE;
        }
        bool selfCopied = false;
        for (uint32_t s = 0; s < 4; s++) {
            if (!pd.surfaceStage[s])
                continue;
            if (pd.surfaceStage[s] == pd.targetColorGeneration) {
                /* The descriptor set binds this stage to the target's
                 * snapshot; fill it with the target's CURRENT content
                 * before the draw, exactly like the geometry replay path.
                 * Without this copy the composite samples an uninitialized
                 * snapshot (hleroam3: white x light erased the scene). */
                if (target && !selfCopied) {
                    transitionSurface(*target, RenderTextureLayout::COPY_SOURCE,
                                      false);
                    transitionSurface(*target, RenderTextureLayout::COPY_DEST,
                                      true);
                    cl->copyTexture(target->snapshot.get(),
                                    target->texture.get());
                    mark("snapshot-prog");
                    transitionSurface(*target, RenderTextureLayout::SHADER_READ,
                                      true);
                    transitionSurface(*target, RenderTextureLayout::COLOR_WRITE,
                                      false);
                    selfCopied = true;
                }
                continue;
            }
            auto it = m_surfaceCache.find(pd.surfaceStage[s]);
            if (it != m_surfaceCache.end()) {
                PlumeColorSurface &sample = it->second;
                if (isBackbufferMirrorGeneration(
                        pd.surfaceStage[s], presentTarget) &&
                    sample.guestAddr == presentTarget &&
                    sample.width == m_outputWidth &&
                    sample.height == m_outputHeight) {
                    /* Device-backbuffer alias: backbuffer draws render into
                     * the backend scene texture, never into this cache
                     * entry, so refresh the mirror from the screen before
                     * sampling.  Xbox reads the live framebuffer memory. */
                    if (screenLayout != RenderTextureLayout::COPY_SOURCE) {
                        cl->barriers(RenderBarrierStage::COPY,
                                     RenderTextureBarrier(
                                         screenTexture,
                                         RenderTextureLayout::COPY_SOURCE));
                        screenLayout = RenderTextureLayout::COPY_SOURCE;
                    }
                    transitionSurface(sample, RenderTextureLayout::COPY_DEST,
                                      false);
                    cl->copyTexture(sample.texture.get(), screenTexture);
                    mark("backbuffer-alias-prog");
                    if (!target) {
                        cl->barriers(RenderBarrierStage::GRAPHICS,
                                     RenderTextureBarrier(
                                         screenTexture,
                                         RenderTextureLayout::COLOR_WRITE));
                        screenLayout = RenderTextureLayout::COLOR_WRITE;
                    }
                }
                transitionSurface(sample, RenderTextureLayout::SHADER_READ,
                                  false);
            }
        }
        cl->setFramebuffer(targetFramebuffer);
        cl->setViewports(RenderViewport(
            0.0f, 0.0f, float(physicalTargetWidth),
            float(physicalTargetHeight)));
        cl->setScissors(drawScissorRect(
            pd.renderState, targetWidth, targetHeight,
            m_internalResolutionScale));
        mark("setfb-prog");

        const uint64_t descT0 = spikeT0 ? xrecomp_host_monotonic_ns() : 0;
        RenderDescriptorSet *set = createProgDrawDescriptorSet(
            ctx, pd, sub.progCB.get(),
            static_cast<uint64_t>(pd.progConstIndex) *
                kProgramConstantStride,
            sub.vsCB.get(), sub.vsCBCap);
        if (descT0) {
            nsDesc += xrecomp_host_monotonic_ns() - descT0;
            descCreates++;
        }
        mark("desc-prog");
        const bool targetHasDepthAttachment =
            target == nullptr || targetZeta != nullptr;
        RenderPipeline *pso = progIdxPso(
            ctx, pd, targetHasDepthAttachment);
        mark("pso-idx");
        if (!set || !pso) {
            if (f2On)
                xgpu_plume_f2_log("p %zu SKIP pso set=%d pso=%d ps=%u",
                                  pi, set != nullptr, pso != nullptr,
                                  pd.psHandle);
            return;
        }
        const ProgrammableUiCanvasTransform uiTransform =
            programmableUiCanvasTransform(
                pd.renderState, targetWidth, targetHeight);
        const ProgramVertexPushConstants pushConstants =
            makeProgramVertexPushConstants(
                targetWidth, targetHeight, 1.0f,
                pd.viewportZOffset, pd.viewportZScale,
                pd.vsConstIndex * 192u,
                uiTransform.horizontal.scale,
                uiTransform.horizontal.offset);
        cl->setGraphicsPipelineLayout(m_progIdxLayout.get());
        cl->setPipeline(pso);
        cl->setGraphicsPushConstants(0, &pushConstants);
        cl->setGraphicsDescriptorSet(set, 0);
        cl->setBlendFactor(
            plume_blend_factor_from_xgpu(pd.renderState.blend_color));
        RenderVertexBufferView vbv(
            RenderBufferReference(sub.rawVbuf.get(), pd.vbufOffset), pd.vbufBytes);
        RenderInputSlot vslot(0, pd.stride);
        cl->setVertexBuffers(0, &vbv, 1, &vslot);
        RenderIndexBufferView ibv(
            RenderBufferReference(sub.ibuf.get(), (uint64_t)pd.ibufOffset * 4u),
            pd.indexCount * 4u, RenderFormat::R32_UINT);
        cl->setIndexBuffer(&ibv);
        cl->drawIndexedInstanced(pd.indexCount, 1, 0, 0, 0);
        mark("draw-idx");
        if (f2On) {
            char stages[192];
            f2Stages(stages, sizeof stages, pd.surfaceStage, pd.stageTexture);
            const uint8_t *rvb =
                pd.vbufOffset + pd.vbufBytes <= rec.frameRawVerts.size()
                    ? rec.frameRawVerts.data() + pd.vbufOffset : nullptr;
            xgpu_plume_f2_log(
                "p %zu after=%u tgt=%08X %ux%u cg=%llu zg=%llu fb=%llu "
                "idx=%u stride=%u topo=%u ps=%u vsk=%016llX attrs=%04X vpz=%g/%g "
                "cc=%u:%llX vscc=%u:%llX st=%llX vh=%llX "
                "msk=%X bl=%u:%u:%u:%u tex=%s",
                pi, pd.afterGeom, pd.targetGuest, pd.targetWidth,
                pd.targetHeight,
                (unsigned long long)pd.targetColorGeneration,
                (unsigned long long)pd.targetZetaGeneration,
                (unsigned long long)pd.targetFramebufferGeneration,
                pd.indexCount, pd.stride, pd.topology, pd.psHandle,
                (unsigned long long)pd.vertexProgramKey, pd.attrsUsed,
                pd.viewportZOffset, pd.viewportZScale,
                pd.progConstIndex,
                pd.progConstIndex < rec.frameProgConsts.size()
                    ? f2Hash(rec.frameProgConsts[pd.progConstIndex].data(),
                             rec.frameProgConsts[pd.progConstIndex].size() *
                                 sizeof(float))
                    : 0ull,
                pd.vsConstIndex,
                f2Hash(rec.frameVSConsts[pd.vsConstIndex].data(),
                       rec.frameVSConsts[pd.vsConstIndex].size() *
                           sizeof(float)),
                f2Hash(&pd.renderState, sizeof pd.renderState),
                rvb ? f2Hash(rvb, pd.vbufBytes) : 0ull,
                pd.renderState.color_write_mask, pd.renderState.blend_enable,
                pd.renderState.src_blend, pd.renderState.dst_blend,
                pd.renderState.blend_op, stages);
        }
        if (m_progDescCache.size() >= kProgDescriptorBatchLimit)
            submitDescriptorBatch();
    };
    size_t progIdx = 0;
    auto flushProgUpTo = [&](size_t geomIndex) {
        while (progIdx < rec.progDraws.size() &&
               rec.progDraws[progIdx].afterGeom <= geomIndex) {
            renderProgDraw(rec.progDraws[progIdx], progIdx);
            progIdx++;
        }
    };

    XRECOMP_TRACY_ZONE_BEGIN(replay_draw_stream_zone,
                            "Plume Replay Draw Stream");
    for (size_t di = 0; di < rec.draws.size(); di++) {
        flushProgUpTo(di);
        const GeomDraw &d = rec.draws[di];
        if (plumeDebugOverlayShouldCopyPresentBeforeDraw(
                d.afterPresentCopy != 0, copyPresentSurface,
                presentTarget != 0, presentSurfaceCopied))
            copyPresentTarget();
        /* 2D-engine surface blit: ordered copy of the source surface's
         * replay-time contents into the destination (display) surface. */
        if (d.blitDstGeneration) {
            auto srcIt = m_surfaceCache.find(d.blitSrcGeneration);
            auto dstIt = m_surfaceCache.find(d.blitDstGeneration);
            if (srcIt == m_surfaceCache.end() ||
                dstIt == m_surfaceCache.end() ||
                srcIt->second.width != dstIt->second.width ||
                srcIt->second.height != dstIt->second.height) {
                if (f2On)
                    xgpu_plume_f2_log("g %zu SKIP blit src=%llu dst=%llu",
                                      di,
                                      (unsigned long long)d.blitSrcGeneration,
                                      (unsigned long long)d.blitDstGeneration);
                continue;
            }
            transitionSurface(srcIt->second, RenderTextureLayout::COPY_SOURCE,
                              false);
            transitionSurface(dstIt->second, RenderTextureLayout::COPY_DEST,
                              false);
            cl->copyTexture(dstIt->second.texture.get(),
                            srcIt->second.texture.get());
            mark("blit");
            transitionSurface(dstIt->second, RenderTextureLayout::SHADER_READ,
                              false);
            if (f2On)
                xgpu_plume_f2_log("g %zu blit src=%llu dst=%llu %ux%u", di,
                                  (unsigned long long)d.blitSrcGeneration,
                                  (unsigned long long)d.blitDstGeneration,
                                  srcIt->second.width, srcIt->second.height);
            continue;
        }
        /* Four dynamic sampler descriptors per programmable set share Plume's
         * finite shader-visible heap. Retire completed sets before replay size
         * can make descriptor demand scale without bound. */
        if (d.psHandle &&
            m_progDescCache.size() >= kProgDescriptorBatchLimit)
            submitDescriptorBatch();

        PlumeColorSurface *target = nullptr;
        PlumeZetaSurface *targetZeta = nullptr;
        RenderTexture *targetTexture = screenTexture;
        RenderFramebuffer *targetFramebuffer = screenFramebuffer;
        uint32_t targetWidth = m_outputWidth;
        uint32_t targetHeight = m_outputHeight;
        if (d.targetGuest) {
            auto targetIt = m_surfaceCache.find(d.targetColorGeneration);
            auto framebufferIt = m_framebufferCache.find(
                d.targetFramebufferGeneration);
            if (targetIt == m_surfaceCache.end() ||
                framebufferIt == m_framebufferCache.end() ||
                targetIt->second.width != d.targetWidth ||
                targetIt->second.height != d.targetHeight) {
                if (f2On)
                    xgpu_plume_f2_log("g %zu SKIP target tgt=%08X %ux%u "
                                      "cg=%llu fb=%llu vtx=%u", di,
                                      d.targetGuest, d.targetWidth,
                                      d.targetHeight,
                                      (unsigned long long)d.targetColorGeneration,
                                      (unsigned long long)d.targetFramebufferGeneration,
                                      d.vertexCount);
                continue;
            }
            target = &targetIt->second;
            if (d.targetZetaGeneration) {
                auto zetaIt = m_zetaCache.find(d.targetZetaGeneration);
                if (zetaIt == m_zetaCache.end()) {
                    if (f2On)
                        xgpu_plume_f2_log("g %zu SKIP zeta tgt=%08X zg=%llu "
                                          "vtx=%u", di, d.targetGuest,
                                          (unsigned long long)d.targetZetaGeneration,
                                          d.vertexCount);
                    continue;
                }
                targetZeta = &zetaIt->second;
            }
            targetTexture = target->texture.get();
            targetFramebuffer = framebufferIt->second.framebuffer.get();
            targetWidth = target->width;
            targetHeight = target->height;
        }
        uint32_t physicalTargetWidth = 0;
        uint32_t physicalTargetHeight = 0;
        if (!plumeScaledExtent(targetWidth, targetHeight,
                               m_internalResolutionScale,
                               &physicalTargetWidth,
                               &physicalTargetHeight))
            continue;

        if (d.clear || d.clearDepth || d.clearStencil || d.vertexCount) {
            if (target) {
                if ((d.clear || d.vertexCount) &&
                    (m_replayWritten.empty() ||
                     m_replayWritten.back() != d.targetColorGeneration))
                    m_replayWritten.push_back(d.targetColorGeneration);
                /* A depth-only clear still binds the complete framebuffer.
                 * The backend requires every bound color attachment to be in
                 * COLOR_WRITE even when this operation will not modify it. */
                transitionSurface(*target, RenderTextureLayout::COLOR_WRITE,
                                  false);
                /* Binding a framebuffer also binds its depth attachment.
                 * Keep the attachment in a legal depth layout even for a
                 * color-only clear or a draw with depth testing disabled. */
                if (targetZeta)
                    transitionDepth(*targetZeta);
            }
            else if (screenLayout != RenderTextureLayout::COLOR_WRITE) {
                cl->barriers(RenderBarrierStage::GRAPHICS,
                             RenderTextureBarrier(screenTexture,
                                                  RenderTextureLayout::COLOR_WRITE));
                screenLayout = RenderTextureLayout::COLOR_WRITE;
            }
            cl->setFramebuffer(targetFramebuffer);
            cl->setViewports(RenderViewport(
                0.0f, 0.0f, float(physicalTargetWidth),
                float(physicalTargetHeight)));
            cl->setScissors(drawScissorRect(
                d.renderState, targetWidth, targetHeight,
                m_internalResolutionScale));
            mark("setfb");
        }
        RenderRect clearRect;
        const RenderRect *clearRects = nullptr;
        uint32_t clearRectCount = 0;
        if (d.hasClearRect) {
            clearRect = RenderRect(
                d.clearRect.x * m_internalResolutionScale,
                d.clearRect.y * m_internalResolutionScale,
                d.clearRect.width * m_internalResolutionScale,
                d.clearRect.height * m_internalResolutionScale);
            clearRects = &clearRect;
            clearRectCount = 1;
        }
        if (d.clear) {
            cl->clearColor(0, RenderColor(d.clearColor[0], d.clearColor[1],
                                          d.clearColor[2], d.clearColor[3]),
                           clearRects, clearRectCount);
            mark("clear");
        }
        const bool targetHasDepthAttachment =
            target == nullptr || targetZeta != nullptr;
        if ((d.clearDepth || d.clearStencil) && targetHasDepthAttachment) {
            cl->clearDepthStencil(d.clearDepth != 0, d.clearStencil != 0,
                                  d.depthClear, d.stencilClear,
                                  clearRects, clearRectCount);
            mark("clear-z");
        } else if (d.clearDepth || d.clearStencil) {
            /* D3D8 titles may retain Z/stencil clear bits while an offscreen
             * color-only target is bound. There is no attachment to clear;
             * forwarding the request violates the RenderCommandList contract
             * and D3D12 aborts on its null DSV. Match the native no-op instead. */
            static bool loggedMissingZetaClear = false;
            if (!loggedMissingZetaClear) {
                std::fprintf(stderr,
                             "[PLUME-RT] ignored depth/stencil clear for "
                             "framebuffer without zeta attachment\n");
                loggedMissingZetaClear = true;
            }
            if (f2On) {
                xgpu_plume_f2_log(
                    "g %zu skip-clear-z tgt=%08X fb=%llu no-zeta", di,
                    d.targetGuest,
                    (unsigned long long)d.targetFramebufferGeneration);
            }
        }
        if (!d.vertexCount) {
            if (f2On && (d.clear || d.clearDepth || d.clearStencil))
                xgpu_plume_f2_log("g %zu clear tgt=%08X cg=%llu c=%u"
                                  "(%.3f,%.3f,%.3f,%.3f) z=%u s=%u rect=%u",
                                  di, d.targetGuest,
                                  (unsigned long long)d.targetColorGeneration,
                                  d.clear, d.clearColor[0], d.clearColor[1],
                                  d.clearColor[2], d.clearColor[3],
                                  d.clearDepth, d.clearStencil,
                                  d.hasClearRect);
            continue;
        }

        bool selfCopied = false;
        for (uint32_t s = 0; s < 4; s++) {
            if (!d.surfaceStage[s])
                continue;
            auto sampleIt = m_surfaceCache.find(d.surfaceStage[s]);
            if (sampleIt == m_surfaceCache.end())
                continue;
            PlumeColorSurface &sample = sampleIt->second;
            if (d.surfaceStage[s] == d.targetColorGeneration && target) {
                if (!selfCopied) {
                    transitionSurface(*target, RenderTextureLayout::COPY_SOURCE,
                                      false);
                    transitionSurface(*target, RenderTextureLayout::COPY_DEST,
                                      true);
                    cl->copyTexture(target->snapshot.get(),
                                    target->texture.get());
                    mark("snapshot");
                    transitionSurface(*target, RenderTextureLayout::SHADER_READ,
                                      true);
                    transitionSurface(*target, RenderTextureLayout::COLOR_WRITE,
                                      false);
                    selfCopied = true;
                }
            } else {
                if (isBackbufferMirrorGeneration(
                        d.surfaceStage[s], presentTarget) &&
                    sample.guestAddr == presentTarget &&
                    sample.width == m_outputWidth &&
                    sample.height == m_outputHeight) {
                    /* Device-backbuffer alias: backbuffer draws render into
                     * the backend scene texture, never into this cache
                     * entry, so refresh the mirror from the screen before
                     * sampling.  Xbox reads the live framebuffer memory. */
                    if (screenLayout != RenderTextureLayout::COPY_SOURCE) {
                        cl->barriers(RenderBarrierStage::COPY,
                                     RenderTextureBarrier(
                                         screenTexture,
                                         RenderTextureLayout::COPY_SOURCE));
                        screenLayout = RenderTextureLayout::COPY_SOURCE;
                    }
                    transitionSurface(sample, RenderTextureLayout::COPY_DEST,
                                      false);
                    cl->copyTexture(sample.texture.get(), screenTexture);
                    mark("backbuffer-alias");
                    if (!target) {
                        cl->barriers(RenderBarrierStage::GRAPHICS,
                                     RenderTextureBarrier(
                                         screenTexture,
                                         RenderTextureLayout::COLOR_WRITE));
                        screenLayout = RenderTextureLayout::COLOR_WRITE;
                    }
                }
                transitionSurface(sample, RenderTextureLayout::SHADER_READ,
                                  false);
            }
        }

        ::plume::RenderBuffer *geomVb = sub.vbuf.get();
        uint32_t geomVbOffset = d.offset;
        uint32_t geomVbBytes = d.byteLen;
        if (d.cachedMeshId) {
            if (d.cachedMeshId >= m_cachedMeshes.size() ||
                !m_cachedMeshes[d.cachedMeshId].vb ||
                !m_cachedMeshes[d.cachedMeshId].ib) {
                if (f2On)
                    xgpu_plume_f2_log("g %zu SKIP cached-mesh", di);
                continue;
            }
            geomVb = m_cachedMeshes[d.cachedMeshId].vb.get();
            geomVbOffset = 0;
            geomVbBytes = m_cachedMeshes[d.cachedMeshId].vbBytes;
        }
        ::plume::RenderVertexBufferView view(
            ::plume::RenderBufferReference(geomVb, geomVbOffset), geomVbBytes);
        ::plume::RenderInputSlot slot(0, d.stride);
        /* Screen-space XYZRHW -> NDC uses active-target half size (not hardcoded 640x480). */
        const PlumeViewportTransform viewport =
            plume_viewport_transform(d.renderState);
        const ProgrammableUiCanvasTransform uiTransform = d.vsHandle
            ? programmableUiCanvasTransform(
                  d.renderState, targetWidth, targetHeight)
            : ProgrammableUiCanvasTransform{};
        const ProgramVertexPushConstants pushConstants =
            makeProgramVertexPushConstants(
                targetWidth, targetHeight,
                d.renderState.position_mode ? 1.0f : 0.0f,
                d.vsHandle ? viewport.offset[2] : 0.0f,
                d.vsHandle ? viewport.scale[2] : 1.0f,
                d.vsHandle ? d.vsConstIndex * 192u : 0u,
                uiTransform.horizontal.scale,
                uiTransform.horizontal.offset,
                plumeScreenSpacePixelCenterOffset(
                    d.renderState.position_mode, d.vsHandle != 0u));
        const char *f2Route = "geom";
        RenderDescriptorSet *progSet = nullptr;
        if (d.psHandle && sub.progCB &&
            d.progConstIndex < rec.frameProgConsts.size() &&
            (!d.vsHandle || (sub.vsCB &&
             d.vsConstIndex < rec.frameVSConsts.size()))) {
            const uint64_t descT0 = spikeT0 ? xrecomp_host_monotonic_ns() : 0;
            progSet = createProgDrawDescriptorSet(
                ctx, d, sub.progCB.get(),
                static_cast<uint64_t>(d.progConstIndex) *
                    kProgramConstantStride,
                d.vsHandle ? sub.vsCB.get() : nullptr,
                d.vsHandle ? sub.vsCBCap : 0);
            if (descT0) {
                nsDesc += xrecomp_host_monotonic_ns() - descT0;
                descCreates++;
            }
        }
        if (d.psHandle && progSet && sub.progCB &&
            d.progConstIndex < rec.frameProgConsts.size() &&
            (!d.vsHandle || (sub.vsCB &&
             d.vsConstIndex < rec.frameVSConsts.size())) && m_progReady) {
            ProgPsoFailure psoFailure;
            ::plume::RenderPipeline *pso = this->progPso(
                                                     ctx, d.psHandle, d.vsHandle,
                                                     d.stride, d.topology,
                                                     d.hasDiffuse, d.hasSpecular,
                                                     d.hasUV, d.texCount,
                                                     d.uvOffset, d.fvf,
                                                     d.renderState,
                                                     targetHasDepthAttachment,
                                                     &psoFailure);
            mark("pso-prog");
            if (!pso) {
                if (f2On) {
                    uint16_t inputs = 0;
                    uint16_t present = 0;
                    auto vsIt = m_vsReg.find(d.vsHandle);
                    if (vsIt != m_vsReg.end()) {
                        inputs = vsIt->second.inputsRead;
                        present = vsIt->second.attributesPresent;
                        if (std::strcmp(psoFailure.reason,
                                        "vertex-shader") == 0) {
                            f2_dump_vertex_shader(
                                d.vsHandle, vsIt->second.hlsl);
                            if (!vsIt->second.diagnostics.empty()) {
                                std::string diagnostics =
                                    vsIt->second.diagnostics;
                                std::replace(
                                    diagnostics.begin(), diagnostics.end(),
                                    '\n', '|');
                                std::replace(
                                    diagnostics.begin(), diagnostics.end(),
                                    '\r', ' ');
                                xgpu_plume_f2_log(
                                    "vshcompile handle=%08X diagnostics=%s",
                                    d.vsHandle, diagnostics.c_str());
                            }
                        }
                    }
                    xgpu_plume_f2_log(
                        "g %zu SKIP pso-prog reason=%s ps=%u vs=%u "
                        "stride=%u inputs=%04X present=%04X attr=%u "
                        "stream=%u fmt=%02X off=%u size=%u",
                        di, psoFailure.reason, d.psHandle, d.vsHandle,
                        d.stride, inputs, present, psoFailure.attr,
                        psoFailure.stream, psoFailure.format,
                        psoFailure.offset, psoFailure.size);
                }
                continue;
            }
            f2Route = "prog";
            cl->setGraphicsPipelineLayout(
                d.vsHandle ? m_progIdxLayout.get() : m_progLayout.get());
            cl->setPipeline(pso);
            cl->setGraphicsPushConstants(0, &pushConstants);
            cl->setGraphicsDescriptorSet(progSet, 0);
        } else if ((d.stageTexture[0].valid() || d.surfaceStage[0]) &&
                   m_texReady &&
                   d.stride >= (uint32_t)d.uvOffset + 8u) {
            ::plume::RenderPipeline *pso = this->texPso(
                ctx, d.stride, d.topology, d.hasDiffuse, d.uvOffset,
                d.renderState, targetHasDepthAttachment);
            mark("pso-tex");
            if (!pso) {
                if (f2On)
                    xgpu_plume_f2_log("g %zu SKIP pso-tex", di);
                continue;
            }
            PlumeTex *boundTexture =
                resolveTextureBinding(d.stageTexture[0]);
            RenderDescriptorSet *set = boundTexture
                ? boundTexture->descSet.get() : nullptr;
            if (d.surfaceStage[0]) {
                auto sampleIt = m_surfaceCache.find(d.surfaceStage[0]);
                if (sampleIt == m_surfaceCache.end()) {
                    if (f2On)
                        xgpu_plume_f2_log("g %zu SKIP sample S%llu", di,
                                          (unsigned long long)d.surfaceStage[0]);
                    continue;
                }
                set = d.surfaceStage[0] == d.targetColorGeneration
                          ? sampleIt->second.snapshotDescSet.get()
                          : sampleIt->second.descSet.get();
            }
            if (!set) {
                if (f2On)
                    xgpu_plume_f2_log(
                        "g %zu SKIP texture-key guest=%08X version=%016llX",
                        di, d.stageTexture[0].guest,
                        (unsigned long long)d.stageTexture[0].version);
                continue;
            }
            f2Route = "tex";
            cl->setGraphicsPipelineLayout(m_texLayout.get());
            cl->setPipeline(pso);
            cl->setGraphicsPushConstants(0, &pushConstants);
            cl->setGraphicsDescriptorSet(set, 0);
        } else {
            ::plume::RenderPipeline *pso = this->geomPso(
                ctx, d.stride, d.topology, d.hasDiffuse, d.renderState,
                targetHasDepthAttachment);
            mark("pso-geom");
            if (!pso) {
                if (f2On)
                    xgpu_plume_f2_log("g %zu SKIP pso-geom", di);
                continue;
            }
            cl->setGraphicsPipelineLayout(m_geomLayout.get());
            cl->setPipeline(pso);
            cl->setGraphicsPushConstants(0, &pushConstants);
        }
        cl->setBlendFactor(
            plume_blend_factor_from_xgpu(d.renderState.blend_color));
        cl->setVertexBuffers(0, &view, 1, &slot);
        if (d.indexCount) {
            ::plume::RenderBuffer *geomIb = sub.ibuf.get();
            uint32_t geomIbOffset = d.indexOffset;
            if (d.cachedMeshId) {
                geomIb = m_cachedMeshes[d.cachedMeshId].ib.get();
                geomIbOffset = 0;
            } else if (!sub.ibuf ||
                d.indexOffset > rec.frameIndices.size() ||
                d.indexCount > rec.frameIndices.size() - d.indexOffset) {
                if (f2On)
                    xgpu_plume_f2_log("g %zu SKIP index-stream", di);
                continue;
            }
            RenderIndexBufferView indexView(
                RenderBufferReference(
                    geomIb, (uint64_t)geomIbOffset * 4u),
                d.indexCount * 4u, RenderFormat::R32_UINT);
            cl->setIndexBuffer(&indexView);
            cl->drawIndexedInstanced(d.indexCount, 1, 0, 0, 0);
            mark("draw-idx-geom");
        } else {
            cl->drawInstanced(d.vertexCount, 1, 0, 0);
            mark("draw");
        }
        if (f2On) {
            char stages[192];
            char samplers[256];
            uint16_t vertexOutputs = 0;
            if (d.vsHandle) {
                auto shader = m_vsReg.find(d.vsHandle);
                if (shader != m_vsReg.end())
                    vertexOutputs = shader->second.outputsWritten;
            }
            f2Stages(stages, sizeof stages, d.surfaceStage, d.stageTexture);
            f2Samplers(samplers, sizeof samplers, d.stageSamplerState,
                       d.stageSamplerStateValid);
            const uint8_t *vb =
                d.offset + d.byteLen <= rec.frameVerts.size()
                    ? rec.frameVerts.data() + d.offset : nullptr;
            xgpu_plume_f2_log(
                "g %zu route=%s tgt=%08X %ux%u cg=%llu zg=%llu fb=%llu "
                "vtx=%u stride=%u topo=%u dif=%u spec=%u uv=%u@%u "
                "aps=%u ps=%u "
                "vs=%u clr=%u cc=%u:%llX vscc=%u:%llX st=%llX vh=%llX "
                "vp=%u,%u,%ux%u z=%.9g:%.9g msk=%X "
                "bl=%u:%u:%u:%u "
                "at=%u:%u:%u "
                "ff0=%u:%u:%u/%u:%u:%u "
                "ff1=%u:%u:%u/%u:%u:%u "
                "ff2=%u:%u:%u/%u:%u:%u "
                "ff3=%u:%u:%u/%u:%u:%u "
                "ffv=%u vout=%04X tf=%08X tex=%s samp=%s",
                di, f2Route, d.targetGuest, d.targetWidth, d.targetHeight,
                (unsigned long long)d.targetColorGeneration,
                (unsigned long long)d.targetZetaGeneration,
                (unsigned long long)d.targetFramebufferGeneration,
                d.vertexCount, d.stride, d.topology, d.hasDiffuse,
                d.hasSpecular, d.hasUV, d.uvOffset, d.activePsHandle,
                d.psHandle, d.vsHandle,
                d.clear, d.progConstIndex,
                d.progConstIndex < rec.frameProgConsts.size()
                    ? f2Hash(rec.frameProgConsts[d.progConstIndex].data(),
                             rec.frameProgConsts[d.progConstIndex].size() *
                                 sizeof(float))
                    : 0ull,
                d.vsConstIndex,
                d.vsConstIndex < rec.frameVSConsts.size()
                    ? f2Hash(rec.frameVSConsts[d.vsConstIndex].data(),
                             rec.frameVSConsts[d.vsConstIndex].size() *
                                 sizeof(float))
                    : 0ull,
                f2Hash(&d.renderState, sizeof d.renderState),
                vb ? f2Hash(vb, d.byteLen) : 0ull,
                d.renderState.viewport_x, d.renderState.viewport_y,
                d.renderState.viewport_width, d.renderState.viewport_height,
                pushConstants.viewportZOffset,
                pushConstants.viewportZScale,
                d.renderState.color_write_mask, d.renderState.blend_enable,
                d.renderState.src_blend, d.renderState.dst_blend,
                d.renderState.blend_op,
                d.renderState.alpha_test_enable,
                d.renderState.alpha_func, d.renderState.alpha_ref,
                d.renderState.fixed_color_op[0],
                d.renderState.fixed_color_arg1[0],
                d.renderState.fixed_color_arg2[0],
                d.renderState.fixed_alpha_op[0],
                d.renderState.fixed_alpha_arg1[0],
                d.renderState.fixed_alpha_arg2[0],
                d.renderState.fixed_color_op[1],
                d.renderState.fixed_color_arg1[1],
                d.renderState.fixed_color_arg2[1],
                d.renderState.fixed_alpha_op[1],
                d.renderState.fixed_alpha_arg1[1],
                d.renderState.fixed_alpha_arg2[1],
                d.renderState.fixed_color_op[2],
                d.renderState.fixed_color_arg1[2],
                d.renderState.fixed_color_arg2[2],
                d.renderState.fixed_alpha_op[2],
                d.renderState.fixed_alpha_arg1[2],
                d.renderState.fixed_alpha_arg2[2],
                d.renderState.fixed_color_op[3],
                d.renderState.fixed_color_arg1[3],
                d.renderState.fixed_color_arg2[3],
                d.renderState.fixed_alpha_op[3],
                d.renderState.fixed_alpha_arg1[3],
                d.renderState.fixed_alpha_arg2[3],
                d.renderState.fixed_state_valid, vertexOutputs,
                d.renderState.fixed_texture_factor, stages, samplers);
            if (d.vsHandle) {
                auto shader = m_vsReg.find(d.vsHandle);
                if (shader != m_vsReg.end()) {
                    f2_dump_vertex_shader(d.vsHandle, shader->second.hlsl);
                    char declaration[512];
                    size_t declarationOffset = 0;
                    for (uint32_t attr = 0; attr < 16u &&
                         declarationOffset + 32u < sizeof(declaration); ++attr) {
                        if (!(shader->second.attributesPresent & (1u << attr)))
                            continue;
                        const int written = std::snprintf(
                            declaration + declarationOffset,
                            sizeof(declaration) - declarationOffset,
                            "%sa%u=s%u/f%02X/o%u",
                            declarationOffset ? "," : "", attr,
                            shader->second.stream[attr],
                            shader->second.format[attr],
                            shader->second.offset[attr]);
                        if (written < 0)
                            break;
                        declarationOffset += static_cast<size_t>(written);
                    }
                    declaration[declarationOffset < sizeof(declaration)
                                    ? declarationOffset
                                    : sizeof(declaration) - 1u] = '\0';
                    xgpu_plume_f2_log(
                        "vsdecl handle=%08X inputs=%04X present=%04X %s",
                        d.vsHandle, shader->second.inputsRead,
                        shader->second.attributesPresent, declaration);
                }
            }
            if (d.psHandle) {
                auto shader = m_psReg.find(d.psHandle);
                if (shader != m_psReg.end()) {
                    f2_dump_pixel_shader(d.psHandle, shader->second.hlsl);
                    if (!shader->second.diagnostics.empty()) {
                        std::string diagnostics =
                            shader->second.diagnostics;
                        std::replace(
                            diagnostics.begin(), diagnostics.end(),
                            '\r', ' ');
                        std::replace(
                            diagnostics.begin(), diagnostics.end(),
                            '\n', ' ');
                        xgpu_plume_f2_log(
                            "pscompile handle=%08X diagnostics=%s",
                            d.psHandle, diagnostics.c_str());
                    }
                }
            }
            if (vb && d.vsHandle && d.stride >= 16u) {
                const uint32_t count = d.vertexCount < 6u
                    ? d.vertexCount : 6u;
                const auto shader = m_vsReg.find(d.vsHandle);
                for (uint32_t v = 0; v < count; ++v) {
                    const uint8_t *bytes =
                        vb + static_cast<size_t>(v) * d.stride;
                    xgpu_plume_f2_log(
                        "pvtx %zu v%u "
                        "%02X%02X%02X%02X %02X%02X%02X%02X "
                        "%02X%02X%02X%02X %02X%02X%02X%02X",
                        di, v,
                        bytes[0], bytes[1], bytes[2], bytes[3],
                        bytes[4], bytes[5], bytes[6], bytes[7],
                        bytes[8], bytes[9], bytes[10], bytes[11],
                        bytes[12], bytes[13], bytes[14], bytes[15]);
                    if (shader == m_vsReg.end())
                        continue;
                    for (uint32_t attr = 3u; attr <= 4u; ++attr) {
                        const uint32_t offset = shader->second.offset[attr];
                        if (!(shader->second.attributesPresent &
                              (1u << attr)) ||
                            offset > d.stride ||
                            16u > d.stride - offset)
                            continue;
                        const uint8_t *attribute = bytes + offset;
                        xgpu_plume_f2_log(
                            "pattr %zu v%u a%u "
                            "%02X%02X%02X%02X %02X%02X%02X%02X "
                            "%02X%02X%02X%02X %02X%02X%02X%02X",
                            di, v, attr,
                            attribute[0], attribute[1],
                            attribute[2], attribute[3],
                            attribute[4], attribute[5],
                            attribute[6], attribute[7],
                            attribute[8], attribute[9],
                            attribute[10], attribute[11],
                            attribute[12], attribute[13],
                            attribute[14], attribute[15]);
                    }
                }
            }
            /* MM3's compact UI batches put S1 position at byte 0 and
             * UB_D3D colour at byte 4.  A font draw contains its black
             * shadow and coloured foreground in one large vertex buffer, so
             * logging only the first quad makes a healthy draw look black.
             * Summarize the complete buffer while F2 is active instead.  The
             * scan is bounded by the bytes already recorded for this draw
             * and emits one line, regardless of vertex count. */
            if (vb && d.vsHandle && d.stride >= 8u && d.vertexCount &&
                static_cast<uint64_t>(d.vertexCount) * d.stride <=
                    d.byteLen) {
                auto shader = m_vsReg.find(d.vsHandle);
                if (shader != m_vsReg.end() &&
                    (shader->second.attributesPresent & 0x3u) == 0x3u &&
                    shader->second.stream[0] == 0u &&
                    shader->second.stream[1] == 0u &&
                    shader->second.format[0] == 0x21u &&
                    shader->second.format[1] == 0x40u &&
                    shader->second.offset[0] + 4u <= d.stride &&
                    shader->second.offset[1] + 4u <= d.stride) {
                    int16_t minX = INT16_MAX, minY = INT16_MAX;
                    int16_t maxX = INT16_MIN, maxY = INT16_MIN;
                    uint32_t black = 0, white = 0, coloured = 0;
                    uint32_t alphaZero = 0, alphaOpaque = 0;
                    uint32_t firstNonBlack = UINT32_MAX;
                    uint32_t firstNonBlackColour = 0;
                    uint32_t longEdges = 0, brokenQuads = 0;
                    const uint32_t positionOffset = shader->second.offset[0];
                    const uint32_t colourOffset = shader->second.offset[1];
                    for (uint32_t v = 0; v < d.vertexCount; ++v) {
                        const uint8_t *bytes =
                            vb + static_cast<size_t>(v) * d.stride;
                        int16_t x, y;
                        uint32_t colour;
                        std::memcpy(&x, bytes + positionOffset, sizeof(x));
                        std::memcpy(&y, bytes + positionOffset + 2u,
                                    sizeof(y));
                        std::memcpy(&colour, bytes + colourOffset,
                                    sizeof(colour));
                        minX = std::min(minX, x);
                        minY = std::min(minY, y);
                        maxX = std::max(maxX, x);
                        maxY = std::max(maxY, y);
                        const uint32_t rgb = colour & 0x00FFFFFFu;
                        const uint32_t alpha = colour >> 24;
                        if (!rgb)
                            ++black;
                        else if (rgb == 0x00FFFFFFu)
                            ++white;
                        else
                            ++coloured;
                        if (!alpha)
                            ++alphaZero;
                        else if (alpha == 0xFFu)
                            ++alphaOpaque;
                        if (rgb && firstNonBlack == UINT32_MAX) {
                            firstNonBlack = v;
                            firstNonBlackColour = colour;
                        }
                    }
                    if (d.topology == static_cast<uint32_t>(
                            RenderPrimitiveTopology::TRIANGLE_LIST)) {
                        for (uint32_t v = 0; v + 2u < d.vertexCount; v += 3u) {
                            int16_t x[3], y[3];
                            for (uint32_t p = 0; p < 3u; ++p) {
                                const uint8_t *bytes = vb +
                                    static_cast<size_t>(v + p) * d.stride +
                                    positionOffset;
                                std::memcpy(&x[p], bytes, sizeof(x[p]));
                                std::memcpy(&y[p], bytes + 2u, sizeof(y[p]));
                            }
                            for (uint32_t p = 0; p < 3u; ++p) {
                                const uint32_t q = (p + 1u) % 3u;
                                const int dx = std::abs(
                                    static_cast<int>(x[p]) - x[q]);
                                const int dy = std::abs(
                                    static_cast<int>(y[p]) - y[q]);
                                if (dx > 4096 || dy > 4096) {
                                    ++longEdges;
                                    break;
                                }
                            }
                        }
                        for (uint32_t v = 0; v + 5u < d.vertexCount; v += 6u) {
                            const uint8_t *v0 = vb +
                                static_cast<size_t>(v) * d.stride +
                                positionOffset;
                            const uint8_t *v2 = vb +
                                static_cast<size_t>(v + 2u) * d.stride +
                                positionOffset;
                            const uint8_t *v3 = vb +
                                static_cast<size_t>(v + 3u) * d.stride +
                                positionOffset;
                            const uint8_t *v4 = vb +
                                static_cast<size_t>(v + 4u) * d.stride +
                                positionOffset;
                            if (std::memcmp(v0, v3, 4u) != 0 ||
                                std::memcmp(v2, v4, 4u) != 0)
                                ++brokenQuads;
                        }
                    }
                    xgpu_plume_f2_log(
                        "pstat %zu bounds=%d,%d:%d,%d "
                        "color=b%u/w%u/c%u a0=%u/a255=%u "
                        "first-color=%u:%08X long=%u broken6=%u",
                        di, minX, minY, maxX, maxY,
                        black, white, coloured, alphaZero, alphaOpaque,
                        firstNonBlack, firstNonBlackColour,
                        longEdges, brokenQuads);
                }
            }
            /* Fullscreen composite quads carry the exposure gain in their
             * vertex colors: dump position/diffuse/specular per vertex. */
            if (vb && d.vertexCount <= 6 && d.hasDiffuse && d.stride >= 24) {
                for (uint32_t v = 0; v < d.vertexCount; v++) {
                    const uint8_t *p = vb + (size_t)v * d.stride;
                    float pos[4];
                    uint32_t dif, spc;
                    std::memcpy(pos, p, sizeof pos);
                    std::memcpy(&dif, p + 16, 4);
                    std::memcpy(&spc, p + 20, 4);
                    xgpu_plume_f2_log(
                        "gvtx %zu v%u pos=(%.2f,%.2f,%.4f,%.6f) dif=%08X "
                        "spec=%08X", di, v, pos[0], pos[1], pos[2], pos[3],
                        dif, spc);
                }
            }
        }
    }
    flushProgUpTo(rec.draws.size());   /* prog draws after the last GeomDraw */
    XRECOMP_TRACY_ZONE_END(replay_draw_stream_zone);
    copyPresentTarget();

    if (spikeT0) {
        mark("present");
        const uint64_t now = xrecomp_host_monotonic_ns();
        const uint64_t total = now - spikeT0;
        static uint32_t spikeLogs;
        if (total > 40u * 1000000u && spikeLogs < 256) {
            spikeLogs++;
            fprintf(stderr,
                    "[PLUME-REPLAY-SPIKE] total_ms=%.1f upload_ms=%.1f "
                    "desc_ms=%.1f(%u) flush_ms=%.1f(%u) present_ms=%.1f "
                    "other_ms=%.1f maxgap_ms=%.1f@%s geom=%zu prog=%zu "
                    "verts_bytes=%zu\n",
                    total / 1e6, nsUpload / 1e6, nsDesc / 1e6, descCreates,
                    nsFlush / 1e6, flushCount, nsPresent / 1e6,
                    (total - nsUpload - nsDesc - nsFlush - nsPresent) / 1e6,
                    maxGap / 1e6, maxGapLabel,
                    rec.draws.size(), rec.progDraws.size(), rec.frameVerts.size());
        }
    }

    clearFrame();
}

void PlumeDraw::releaseSubmittedResources()
{
    m_progDescCache.clear();
    m_textures.discardRetiredResources();
    m_outputScaleDescriptors.clear();
}

void PlumeDraw::releaseOutputScaleDescriptors()
{
    m_outputScaleDescriptors.clear();
}

bool PlumeDraw::recordOutputScale(
    PlumeContext &ctx, ::plume::RenderCommandList *cmdList,
    ::plume::RenderTexture *source,
    ::plume::RenderTextureView *sourceView,
    ::plume::RenderFramebuffer *destinationFramebuffer,
    uint32_t destinationWidth, uint32_t destinationHeight)
{
    if (!m_outputScaleReady || !cmdList || !source || !sourceView ||
        !destinationFramebuffer || !destinationWidth || !destinationHeight)
        return false;

    const RenderSampler *immutableSampler = m_outputScaleSampler.get();
    RenderDescriptorRange ranges[2] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1,
                              &immutableSampler),
    };
    RenderDescriptorSetDesc descriptorDesc(ranges, 2);
    std::unique_ptr<RenderDescriptorSet> descriptor =
        ctx.device()->createDescriptorSet(descriptorDesc);
    if (!descriptor)
        return false;
    descriptor->setTexture(0, source, RenderTextureLayout::SHADER_READ,
                           sourceView);

    cmdList->setFramebuffer(destinationFramebuffer);
    cmdList->setViewports(RenderViewport(
        0.0f, 0.0f, static_cast<float>(destinationWidth),
        static_cast<float>(destinationHeight)));
    cmdList->setScissors(RenderRect(
        0, 0, destinationWidth, destinationHeight));
    cmdList->setGraphicsPipelineLayout(m_outputScaleLayout.get());
    cmdList->setPipeline(m_outputScalePso.get());
    cmdList->setGraphicsDescriptorSet(descriptor.get(), 0);
    cmdList->drawInstanced(3, 1, 0, 0);
    m_outputScaleDescriptors.push_back(std::move(descriptor));
    return true;
}

bool PlumeDraw::recordHostOutputOverlays(
    PlumeContext &ctx, ::plume::RenderCommandList *cmdList,
    ::plume::RenderFramebuffer *destinationFramebuffer,
    uint32_t destinationWidth, uint32_t destinationHeight)
{
    if (m_hostOutputOverlays.empty())
        return true;
    if (!m_outputScaleReady || !cmdList || !destinationFramebuffer ||
        !destinationWidth || !destinationHeight) {
        m_hostOutputOverlays.clear();
        return false;
    }

    const RenderSampler *immutableSampler = m_outputScaleSampler.get();
    RenderDescriptorRange ranges[2] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1,
                              &immutableSampler),
    };
    RenderDescriptorSetDesc descriptorDesc(ranges, 2);
    bool success = true;
    bool drew = false;
    for (const HostOutputOverlay &overlay : m_hostOutputOverlays) {
        if (!overlay.width || !overlay.height ||
            overlay.x >= destinationWidth || overlay.y >= destinationHeight ||
            overlay.width > destinationWidth - overlay.x ||
            overlay.height > destinationHeight - overlay.y) {
            success = false;
            continue;
        }
        PlumeTex *texture = resolveTextureBinding(overlay.binding);
        if (!texture || !texture->tex || !texture->view) {
            success = false;
            continue;
        }
        std::unique_ptr<RenderDescriptorSet> descriptor =
            ctx.device()->createDescriptorSet(descriptorDesc);
        if (!descriptor) {
            success = false;
            continue;
        }
        descriptor->setTexture(0, texture->tex.get(),
                               RenderTextureLayout::SHADER_READ,
                               texture->view.get());

        cmdList->setFramebuffer(destinationFramebuffer);
        cmdList->setViewports(RenderViewport(
            static_cast<float>(overlay.x), static_cast<float>(overlay.y),
            static_cast<float>(overlay.width),
            static_cast<float>(overlay.height)));
        cmdList->setScissors(RenderRect(
            static_cast<int32_t>(overlay.x),
            static_cast<int32_t>(overlay.y),
            static_cast<int32_t>(overlay.x + overlay.width),
            static_cast<int32_t>(overlay.y + overlay.height)));
        cmdList->setGraphicsPipelineLayout(m_outputScaleLayout.get());
        cmdList->setPipeline(m_outputOverlayPso.get());
        cmdList->setGraphicsDescriptorSet(descriptor.get(), 0);
        cmdList->drawInstanced(3, 1, 0, 0);
        m_outputScaleDescriptors.push_back(std::move(descriptor));
        drew = true;
    }
    if (drew) {
        static bool reported = false;
        if (!reported) {
            const HostOutputOverlay &first = m_hostOutputOverlays.front();
            std::fprintf(stderr,
                         "[PLUME] native host overlay active "
                         "(%ux%u at %u,%u)\n",
                         first.width, first.height, first.x, first.y);
            reported = true;
        }
    }
    m_hostOutputOverlays.clear();
    return success;
}

/* Xbox VRAM is plain RAM: games CPU-read small render targets (MM3's 40x30
 * exposure-measurement ring drives its per-frame tonemap gain). Copy every
 * small color surface the just-recorded replay wrote into a readback buffer
 * on the same command list; after the caller's fence, write it to guest RAM. */
void PlumeDraw::recordSurfaceDownloads(PlumeContext &ctx,
                                       ::plume::RenderCommandList *cl)
{
    static const uint64_t kMaxPixels = 256u * 256u;
    for (uint64_t gen : m_replayWritten) {
        auto it = m_surfaceCache.find(gen);
        if (it == m_surfaceCache.end())
            continue;
        PlumeColorSurface &s = it->second;
        if (!s.guestAddr || !s.guestPitch || !s.width || !s.height ||
            (uint64_t)s.width * s.height > kMaxPixels)
            continue;
        const uint32_t guestRowBytes =
            xgpu_plume_guest_color_row_bytes(s.guestFormat, s.width);
        if (!guestRowBytes || s.guestPitch < guestRowBytes)
            continue;
        if (!xbox_guest_phys_ptr(
                s.guestAddr, static_cast<size_t>(s.guestPitch) * s.height))
            continue;
        if (s.physicalWidth > (UINT32_MAX - 255u) / 4u)
            continue;
        const uint32_t rowPitch =
            (s.physicalWidth * 4u + 255u) & ~255u;
        if (!s.downloadBuffer) {
            s.downloadBuffer = ctx.device()->createBuffer(
                RenderBufferDesc::ReadbackBuffer(uint64_t(rowPitch) *
                                                 s.physicalHeight));
            if (!s.downloadBuffer)
                continue;
        }
        if (s.layout != RenderTextureLayout::COPY_SOURCE) {
            cl->barriers(RenderBarrierStage::COPY,
                         RenderTextureBarrier(
                             s.texture.get(),
                             RenderTextureLayout::COPY_SOURCE));
            s.layout = RenderTextureLayout::COPY_SOURCE;
        }
        const RenderTextureCopyLocation dst =
            RenderTextureCopyLocation::PlacedFootprint(
                s.downloadBuffer.get(), RenderFormat::B8G8R8A8_UNORM,
                s.physicalWidth, s.physicalHeight,
                1, rowPitch / 4u);
        const RenderTextureCopyLocation src =
            RenderTextureCopyLocation::Subresource(s.texture.get(), 0, 0);
        cl->copyTextureRegion(dst, src);
        m_pendingDownloads.push_back(
            PendingSurfaceDownload{gen, s.contentSerial});
    }
    m_replayWritten.clear();
}

void PlumeDraw::completeSurfaceDownloads()
{
    completeDownloadsFrom(m_pendingDownloads);
    m_pendingDownloads.clear();
}

void PlumeDraw::deferSurfaceDownloads()
{
    m_deferredPresentDownloads.insert(m_deferredPresentDownloads.end(),
                                      m_pendingDownloads.begin(),
                                      m_pendingDownloads.end());
    m_pendingDownloads.clear();
}

void PlumeDraw::completeDeferredSurfaceDownloads()
{
    completeDownloadsFrom(m_deferredPresentDownloads);
    m_deferredPresentDownloads.clear();
}

/* Retain the just-submitted async batch's in-flight resources on slot idx; the
 * shared members are cleared so the next batch starts fresh. Freed by
 * reclaimSub once the slot's fence signals. */
void PlumeDraw::retireSub(uint32_t idx)
{
    if (idx >= kWaitRingSize)
        return; /* present slot is synchronous; nothing to defer */
    Submission &s = m_sub[idx];
    for (auto &kv : m_progDescCache)
        s.descBucket.push_back(std::move(kv.second));
    m_progDescCache.clear();
    m_textures.releaseRetiredResources(s.retiredBucket);
    s.downloadBucket.insert(s.downloadBucket.end(),
                            m_pendingDownloads.begin(),
                            m_pendingDownloads.end());
    m_pendingDownloads.clear();
}

/* Slot idx's GPU work is known complete: flush its guest-RAM downloads and free
 * its retained descriptors/textures. */
void PlumeDraw::reclaimSub(uint32_t idx)
{
    if (idx >= kWaitRingSize)
        return;
    Submission &s = m_sub[idx];
    completeDownloadsFrom(s.downloadBucket);
    s.downloadBucket.clear();
    s.descBucket.clear();
    s.retiredBucket.clear();
}

void PlumeDraw::completeDownloadsFrom(
    const std::vector<PendingSurfaceDownload> &downloads)
{
    static uint32_t downloadLogs = 0;
    for (const PendingSurfaceDownload &download : downloads) {
        const uint64_t gen = download.generation;
        auto it = m_surfaceCache.find(gen);
        if (it == m_surfaceCache.end() || !it->second.downloadBuffer)
            continue;
        PlumeColorSurface &s = it->second;
        if (s.physicalWidth > (UINT32_MAX - 255u) / 4u)
            continue;
        const uint32_t rowPitch =
            (s.physicalWidth * 4u + 255u) & ~255u;
        const RenderRange range(
            0, uint64_t(rowPitch) * s.physicalHeight);
        const uint8_t *mapped = static_cast<const uint8_t *>(
            s.downloadBuffer->map(0, &range));
        if (!mapped)
            continue;
        const uint32_t logicalPitch = s.width * 4u;
        std::vector<uint8_t> logical(
            static_cast<size_t>(logicalPitch) * s.height);
        if (!plumeScaleColorBgra8Linear(
                mapped, s.physicalWidth, s.physicalHeight, rowPitch,
                logical.data(), s.width, s.height, logicalPitch)) {
            s.downloadBuffer->unmap();
            continue;
        }
        const uint32_t guestRowBytes =
            xgpu_plume_guest_color_row_bytes(s.guestFormat, s.width);
        uint8_t *guest = xbox_guest_phys_ptr(
            s.guestAddr, static_cast<size_t>(s.guestPitch) * s.height);
        if (!guest) {
            s.downloadBuffer->unmap();
            continue;
        }
        std::vector<uint8_t> packedLinear;
        uint8_t *packTarget = guest;
        uint32_t packPitch = s.guestPitch;
        if (s.guestLayout == XGPU_SURFACE_SWIZZLE) {
            packedLinear.resize(
                static_cast<size_t>(guestRowBytes) * s.height);
            packTarget = packedLinear.data();
            packPitch = guestRowBytes;
        }
        const bool packed = xgpu_plume_pack_guest_color_surface(
            s.guestFormat, logical.data(), logicalPitch,
            packTarget, packPitch,
            s.width, s.height) != 0;
        if (packed && s.guestLayout == XGPU_SURFACE_SWIZZLE) {
            xbox_swizzle_rect(guest, packedLinear.data(), s.width, s.height,
                              guestRowBytes / s.width);
        }
        if (xgpu_plume_f2_active()) {
            /* Exposure-loop visibility: the game CPU-reads these small
             * surfaces to drive its next-frame gain. */
            uint64_t sum[4] = {0, 0, 0, 0};
            for (uint32_t row = 0; row < s.height; row++) {
                const uint8_t *px =
                    logical.data() + (size_t)row * logicalPitch;
                for (uint32_t x = 0; x < s.width; x++) {
                    sum[0] += px[x * 4 + 0];
                    sum[1] += px[x * 4 + 1];
                    sum[2] += px[x * 4 + 2];
                    sum[3] += px[x * 4 + 3];
                }
            }
            const uint64_t n = (uint64_t)s.width * s.height;
            xgpu_plume_f2_log("download guest=%08X %ux%u fmt=%02X gen=%llu "
                              "avg_bgra=%llu,%llu,%llu,%llu",
                              s.guestAddr, s.width, s.height, s.guestFormat,
                              (unsigned long long)gen,
                              (unsigned long long)(sum[0] / n),
                              (unsigned long long)(sum[1] / n),
                              (unsigned long long)(sum[2] / n),
                              (unsigned long long)(sum[3] / n));
        }
        s.downloadBuffer->unmap();
        if (!packed)
            continue;
        /* Guest RAM now holds this surface's bytes through the content
         * serial the copy command observed; a newer serial from a later
         * queued copy must not regress. */
        if (s.resolvedSerial < download.contentSerial)
            s.resolvedSerial = download.contentSerial;
        if (downloadLogs < 16) {
            downloadLogs++;
            fprintf(stderr,
                    "[PLUME-DOWNLOAD] guest=%08X %ux%u pitch=%u fmt=%02X "
                    "gen=%llu\n",
                    s.guestAddr, s.width, s.height, s.guestPitch,
                    s.guestFormat,
                    (unsigned long long)gen);
        }
    }
}

} /* namespace plume */
} /* namespace xgpu */
