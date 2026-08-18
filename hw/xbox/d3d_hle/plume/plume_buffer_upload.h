#ifndef XGPU_PLUME_BUFFER_UPLOAD_H
#define XGPU_PLUME_BUFFER_UPLOAD_H

#include "../platform/xrecomp_tracy.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "plume_render_interface.h"

namespace xgpu {
namespace plume {

struct LatchedVertexStream {
    uint32_t vertexOffset = 0;
    uint32_t vertexBytes = 0;
    uint32_t stride = 0;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    uint32_t latchOffsets[16] = {};
};

struct LatchedVertexArray {
    uint32_t vertexBytes = 0;
    uint32_t stride = 0;
    uint32_t latchOffsets[16] = {};
};

inline bool materializeLatchedVertexArray(
    std::vector<uint8_t> &destinationVertices,
    const void *sourceVertices, uint32_t vertexCount, uint32_t sourceStride,
    uint16_t latchMask, const float *latchedInputs,
    LatchedVertexArray *array)
{
    if (!sourceVertices || !latchedInputs || !array || sourceStride == 0 ||
        vertexCount == 0 || latchMask == 0)
        return false;

    uint32_t latchCount = 0;
    for (uint32_t attr = 0; attr < 16; attr++)
        if (latchMask & (uint16_t)(1u << attr))
            latchCount++;

    const uint32_t latchBytes = latchCount * 4u * sizeof(float);
    if (sourceStride > std::numeric_limits<uint32_t>::max() - latchBytes)
        return false;
    const uint32_t stagedStride = sourceStride + latchBytes;
    const uint64_t stagedBytes64 =
        (uint64_t)vertexCount * (uint64_t)stagedStride;
    if (stagedBytes64 > std::numeric_limits<uint32_t>::max() ||
        stagedBytes64 > std::numeric_limits<size_t>::max())
        return false;

    LatchedVertexArray result = {};
    result.vertexBytes = (uint32_t)stagedBytes64;
    result.stride = stagedStride;

    uint32_t appendedOffset = sourceStride;
    for (uint32_t attr = 0; attr < 16; attr++) {
        if (!(latchMask & (uint16_t)(1u << attr)))
            continue;
        result.latchOffsets[attr] = appendedOffset;
        appendedOffset += 4u * sizeof(float);
    }

    destinationVertices.resize((size_t)result.vertexBytes);
    uint8_t *staged = destinationVertices.data();
    const uint8_t *source = static_cast<const uint8_t *>(sourceVertices);
    for (uint32_t i = 0; i < vertexCount; i++) {
        uint8_t *vertex = staged + (size_t)i * stagedStride;
        std::memcpy(vertex, source + (size_t)i * sourceStride, sourceStride);
        for (uint32_t attr = 0; attr < 16; attr++) {
            if (!(latchMask & (uint16_t)(1u << attr)))
                continue;
            std::memcpy(vertex + result.latchOffsets[attr],
                        latchedInputs + attr * 4u, 4u * sizeof(float));
        }
    }

    *array = result;
    return true;
}

inline bool materializeLatchedVertexStream(
    std::vector<uint8_t> &destinationVertices,
    std::vector<uint32_t> &destinationIndices,
    const void *sourceVertices, size_t sourceBytes, uint32_t sourceStride,
    const uint32_t *sourceIndices, uint32_t indexCount, uint16_t latchMask,
    const float *latchedInputs, LatchedVertexStream *stream)
{
    if (!sourceVertices || !sourceIndices || !latchedInputs || !stream ||
        sourceStride == 0 || indexCount == 0 || latchMask == 0)
        return false;

    uint32_t latchCount = 0;
    for (uint32_t attr = 0; attr < 16; attr++)
        if (latchMask & (uint16_t)(1u << attr))
            latchCount++;

    const uint32_t latchBytes = latchCount * 4u * sizeof(float);
    if (sourceStride > std::numeric_limits<uint32_t>::max() - latchBytes)
        return false;
    const uint32_t stagedStride = sourceStride + latchBytes;

    const uint64_t stagedBytes64 =
        (uint64_t)indexCount * (uint64_t)stagedStride;
    if (stagedBytes64 > std::numeric_limits<uint32_t>::max())
        return false;
    const uint32_t stagedBytes = (uint32_t)stagedBytes64;

    for (uint32_t i = 0; i < indexCount; i++) {
        const uint64_t sourceOffset =
            (uint64_t)sourceIndices[i] * (uint64_t)sourceStride;
        if (sourceOffset + sourceStride > sourceBytes)
            return false;
    }

    if (destinationVertices.size() >
        std::numeric_limits<size_t>::max() - 15u)
        return false;
    const size_t vertexOffset =
        (destinationVertices.size() + 15u) & ~size_t(15u);
    if (vertexOffset > std::numeric_limits<uint32_t>::max() ||
        stagedBytes > std::numeric_limits<size_t>::max() - vertexOffset)
        return false;
    if (destinationIndices.size() >
            std::numeric_limits<uint32_t>::max() ||
        indexCount >
            std::numeric_limits<size_t>::max() - destinationIndices.size())
        return false;

    LatchedVertexStream result = {};
    result.vertexOffset = (uint32_t)vertexOffset;
    result.vertexBytes = stagedBytes;
    result.stride = stagedStride;
    result.indexOffset = (uint32_t)destinationIndices.size();
    result.indexCount = indexCount;

    uint32_t appendedOffset = sourceStride;
    for (uint32_t attr = 0; attr < 16; attr++) {
        if (!(latchMask & (uint16_t)(1u << attr)))
            continue;
        result.latchOffsets[attr] = appendedOffset;
        appendedOffset += 4u * sizeof(float);
    }

    destinationVertices.resize(vertexOffset + stagedBytes);
    uint8_t *staged = destinationVertices.data() + vertexOffset;
    const uint8_t *source = static_cast<const uint8_t *>(sourceVertices);
    for (uint32_t i = 0; i < indexCount; i++) {
        uint8_t *vertex = staged + (size_t)i * stagedStride;
        const uint8_t *sourceVertex =
            source + (size_t)sourceIndices[i] * sourceStride;
        std::memcpy(vertex, sourceVertex, sourceStride);
        for (uint32_t attr = 0; attr < 16; attr++) {
            if (!(latchMask & (uint16_t)(1u << attr)))
                continue;
            std::memcpy(vertex + result.latchOffsets[attr],
                        latchedInputs + attr * 4u, 4u * sizeof(float));
        }
    }

    destinationIndices.reserve(destinationIndices.size() + indexCount);
    for (uint32_t i = 0; i < indexCount; i++)
        destinationIndices.push_back(i);

    *stream = result;
    return true;
}

inline bool copyToMappedBuffer(::plume::RenderBuffer *buffer,
                               const void *source, size_t bytes)
{
    if (!buffer || !source || bytes == 0)
        return false;

    void *mapped = nullptr;
    {
        XRECOMP_TRACY_ZONE_SCOPED("Plume Buffer Map");
        mapped = buffer->map();
    }
    if (!mapped)
        return false;

    {
        XRECOMP_TRACY_ZONE_SCOPED("Plume Buffer Memcpy");
        std::memcpy(mapped, source, bytes);
    }
    {
        XRECOMP_TRACY_ZONE_SCOPED("Plume Buffer Unmap");
        buffer->unmap();
    }
    return true;
}

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_BUFFER_UPLOAD_H */
