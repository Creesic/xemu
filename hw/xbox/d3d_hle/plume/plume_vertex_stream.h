#ifndef XGPU_PLUME_VERTEX_STREAM_H
#define XGPU_PLUME_VERTEX_STREAM_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace xgpu {
namespace plume {

/*
 * GeomDraw stores byte offsets and lengths as uint32_t, but the backing
 * vector is intentionally dynamic. Reject only streams that cannot be
 * represented by that recorded-draw ABI; a small fixed per-submission cap
 * silently drops valid late draws in geometry-heavy views.
 */
inline bool plumeGrowVertexStream(std::vector<uint8_t> &stream,
                                  size_t byteCount, uint32_t *offset)
{
    if (!offset ||
        stream.size() > std::numeric_limits<uint32_t>::max() ||
        byteCount >
            std::numeric_limits<uint32_t>::max() - stream.size())
        return false;

    const size_t oldSize = stream.size();
    try {
        stream.resize(oldSize + byteCount);
    } catch (...) {
        return false;
    }
    *offset = static_cast<uint32_t>(oldSize);
    return true;
}

inline bool plumeAppendVertexBytes(std::vector<uint8_t> &stream,
                                   const void *bytes, size_t byteCount,
                                   uint32_t *offset)
{
    if (byteCount && !bytes)
        return false;
    if (!plumeGrowVertexStream(stream, byteCount, offset))
        return false;
    if (byteCount)
        std::memcpy(stream.data() + *offset, bytes, byteCount);
    return true;
}

} // namespace plume
} // namespace xgpu

#endif
