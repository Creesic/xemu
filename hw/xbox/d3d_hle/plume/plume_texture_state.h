/*
 * Shared Xbox texture-state normalization for the Plume frontend.
 *
 * The NV2A path supplies packed register values while direct D3D HLE supplies
 * individual D3D texture-stage values.  Both are normalized into the explicit
 * XgpuSamplerBinding contract before a host sampler is created.
 */
#ifndef XGPU_PLUME_TEXTURE_STATE_H
#define XGPU_PLUME_TEXTURE_STATE_H

#include "plume_host.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xgpu {
namespace plume {

struct PlumeFvfTexcoordLayout {
    uint8_t count = 0;
    std::array<uint8_t, 4> components = {};
    std::array<uint16_t, 4> offsets = {};
    uint16_t byteLength = 0;
};

XgpuSamplerBinding plumeDecodeNv2aSampler(uint32_t stage, uint32_t address,
                                          uint32_t filter,
                                          uint32_t border_color);

/*
 * Xbox 3D S3TC stores each 4-slice Z group inside every XY block:
 * z-group -> block-y -> block-x -> slice-within-group. This differs from the
 * ordinary host slice-major order and must be applied before BC3 decoding.
 * Returns SIZE_MAX for coordinates outside the level.
 */
size_t plumeBc3VolumeBlockIndex(
    uint32_t width, uint32_t height, uint32_t depth,
    uint32_t block_x, uint32_t block_y, uint32_t z);

/*
 * D3DFVF texture coordinates have a per-set component count in bits 16..23.
 * The encoding is unusual: 0,1,2,3 mean 2D,3D,4D,1D respectively.
 */
bool plumeDecodeFvfTexcoordLayout(
    uint32_t fvf, uint32_t base_offset, uint32_t stride,
    PlumeFvfTexcoordLayout &layout);

/*
 * X8R8G8B8's X byte is not texture alpha.  Host B8G8R8A8 sampling must see
 * alpha 1 even when guest memory contains zero or scratch data in that byte.
 * Returns true and owns a normalized copy for the two X8 formats; returns
 * false without touching destination for every other format.
 */
bool plumeCopyX8UploadWithOpaqueAlpha(uint32_t format, const void *pixels,
                                     size_t bytes,
                                     std::vector<uint8_t> &destination);

/*
 * Compare only the fields that determine a cached host texture's contents and
 * sampling dimensionality. Stage, guest pointer, source pointer, and pitch are
 * frontend routing details; the guest pointer selects the cache entry before
 * this comparison and a version change represents new source contents.
 */
bool plumeTextureBindingMatchesCached(
    const XgpuTextureBinding &cached,
    const XgpuTextureBinding &requested);

/*
 * Host APIs sample normalized coordinates, while Xbox pitch-linear textures
 * and render-target aliases consume texel coordinates.
 */
std::array<float, 4> plumeTextureCoordinateScale(
    uint32_t width, uint32_t height, uint32_t depth,
    uint32_t unnormalized_coords);

} /* namespace plume */
} /* namespace xgpu */

#endif
