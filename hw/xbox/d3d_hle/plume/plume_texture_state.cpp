#include "plume_texture_state.h"

#include <cstring>

namespace xgpu {
namespace plume {

size_t plumeBc3VolumeBlockIndex(
    uint32_t width, uint32_t height, uint32_t depth,
    uint32_t block_x, uint32_t block_y, uint32_t z)
{
    const uint32_t blocksX = (width + 3u) / 4u;
    const uint32_t blocksY = (height + 3u) / 4u;
    if (!width || !height || !depth ||
        block_x >= blocksX || block_y >= blocksY || z >= depth)
        return SIZE_MAX;

    const uint32_t zGroup = z / 4u;
    const uint32_t groupBaseZ = zGroup * 4u;
    const uint32_t groupDepth =
        depth - groupBaseZ < 4u ? depth - groupBaseZ : 4u;
    return static_cast<size_t>(zGroup) * blocksX * blocksY * 4u +
           static_cast<size_t>(block_y * blocksX + block_x) * groupDepth +
           (z - groupBaseZ);
}

bool plumeDecodeFvfTexcoordLayout(
    uint32_t fvf, uint32_t base_offset, uint32_t stride,
    PlumeFvfTexcoordLayout &layout)
{
    static constexpr uint8_t kComponents[4] = {2, 3, 4, 1};
    const uint32_t count = (fvf >> 8) & 0xFu;
    uint32_t offset = base_offset;

    layout = {};
    if (count > layout.components.size() || base_offset > stride)
        return false;
    layout.count = static_cast<uint8_t>(count);
    for (uint32_t stage = 0; stage < count; ++stage) {
        const uint32_t encoded = (fvf >> (16u + stage * 2u)) & 3u;
        const uint32_t components = kComponents[encoded];
        const uint32_t bytes = components * sizeof(float);
        if (offset > stride || bytes > stride - offset) {
            layout = {};
            return false;
        }
        layout.components[stage] = static_cast<uint8_t>(components);
        layout.offsets[stage] = static_cast<uint16_t>(offset);
        offset += bytes;
    }
    layout.byteLength = static_cast<uint16_t>(offset - base_offset);
    return true;
}

XgpuSamplerBinding plumeDecodeNv2aSampler(uint32_t stage, uint32_t address,
                                          uint32_t filter,
                                          uint32_t border_color)
{
    XgpuSamplerBinding binding = {};
    const uint32_t minFilter = (filter >> 16) & 0x3Fu;
    const uint32_t magFilter = (filter >> 24) & 0xFu;
    int32_t lodBias = (int32_t)(filter & 0x1FFFu);

    if (lodBias & 0x1000)
        lodBias |= ~0x1FFF;

    binding.stage = stage;
    binding.address_u = (address >> 0) & 7u;
    binding.address_v = (address >> 8) & 7u;
    binding.address_w = (address >> 16) & 7u;
    if (!binding.address_u)
        binding.address_u = XGPU_SAMPLER_ADDRESS_WRAP;
    if (!binding.address_v)
        binding.address_v = XGPU_SAMPLER_ADDRESS_WRAP;
    if (!binding.address_w)
        binding.address_w = XGPU_SAMPLER_ADDRESS_WRAP;

    switch (minFilter) {
    case 1: /* BOX_LOD0 */
        binding.min_filter = XGPU_SAMPLER_FILTER_POINT;
        binding.mip_filter = XGPU_SAMPLER_FILTER_NONE;
        break;
    case 2: /* TENT_LOD0 */
        binding.min_filter = XGPU_SAMPLER_FILTER_LINEAR;
        binding.mip_filter = XGPU_SAMPLER_FILTER_NONE;
        break;
    case 3: /* BOX_NEARESTLOD */
        binding.min_filter = XGPU_SAMPLER_FILTER_POINT;
        binding.mip_filter = XGPU_SAMPLER_FILTER_POINT;
        break;
    case 4: /* TENT_NEARESTLOD */
        binding.min_filter = XGPU_SAMPLER_FILTER_LINEAR;
        binding.mip_filter = XGPU_SAMPLER_FILTER_POINT;
        break;
    case 5: /* BOX_TENT_LOD */
        binding.min_filter = XGPU_SAMPLER_FILTER_POINT;
        binding.mip_filter = XGPU_SAMPLER_FILTER_LINEAR;
        break;
    case 6: /* TENT_TENT_LOD */
        binding.min_filter = XGPU_SAMPLER_FILTER_LINEAR;
        binding.mip_filter = XGPU_SAMPLER_FILTER_LINEAR;
        break;
    default:
        binding.min_filter = XGPU_SAMPLER_FILTER_LINEAR;
        binding.mip_filter = XGPU_SAMPLER_FILTER_LINEAR;
        break;
    }
    binding.mag_filter = magFilter == 1
        ? XGPU_SAMPLER_FILTER_POINT : XGPU_SAMPLER_FILTER_LINEAR;
    binding.mip_lod_bias = (float)lodBias / 256.0f;
    binding.max_anisotropy = 1;
    binding.border_color = border_color;
    return binding;
}

bool plumeCopyX8UploadWithOpaqueAlpha(uint32_t format, const void *pixels,
                                     size_t bytes,
                                     std::vector<uint8_t> &destination)
{
    if (format != 0x07u && format != 0x1Eu)
        return false;

    destination.resize(bytes);
    if (bytes)
        std::memcpy(destination.data(), pixels, bytes);
    for (size_t alpha = 3; alpha < bytes; alpha += 4)
        destination[alpha] = 0xFFu;
    return true;
}

bool plumeTextureBindingMatchesCached(
    const XgpuTextureBinding &cached,
    const XgpuTextureBinding &requested)
{
    return cached.width == requested.width &&
           cached.height == requested.height &&
           cached.depth == requested.depth &&
           cached.levels == requested.levels &&
           cached.dimensionality == requested.dimensionality &&
           cached.bytes == requested.bytes &&
           cached.format == requested.format &&
           cached.version == requested.version &&
           cached.cube == requested.cube &&
           cached.unnormalized_coords == requested.unnormalized_coords;
}

std::array<float, 4> plumeTextureCoordinateScale(
    uint32_t width, uint32_t height, uint32_t depth,
    uint32_t unnormalized_coords)
{
    std::array<float, 4> scale = {1.0f, 1.0f, 1.0f, 1.0f};
    if (!unnormalized_coords || !width || !height || !depth)
        return scale;
    scale[0] = 1.0f / static_cast<float>(width);
    scale[1] = 1.0f / static_cast<float>(height);
    scale[2] = 1.0f / static_cast<float>(depth);
    return scale;
}

} /* namespace plume */
} /* namespace xgpu */
