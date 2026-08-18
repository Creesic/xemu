#ifndef XGPU_PLUME_FRAME_CONSTANTS_H
#define XGPU_PLUME_FRAME_CONSTANTS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace xgpu {
namespace plume {

/* Fast path for constants whose source storage is versioned independently of
 * the frame recording.  The auxiliary key covers derived values (the Xbox
 * viewport constants for vertex shaders) that are not part of that source
 * version.  Exact bit comparison preserves the interner's existing behavior
 * for signed zero and NaN payloads. */
template <size_t KeyFloatCount>
struct FrameConstantVersionCache {
    bool valid = false;
    uint64_t version = 0;
    std::array<float, KeyFloatCount> key = {};
    uint32_t index = 0;

    bool lookup(uint64_t candidateVersion,
                const std::array<float, KeyFloatCount> &candidateKey,
                uint32_t &result) const
    {
        if (!valid || version != candidateVersion ||
            std::memcmp(key.data(), candidateKey.data(),
                        key.size() * sizeof(float)) != 0)
            return false;
        result = index;
        return true;
    }

    void remember(uint64_t candidateVersion,
                  const std::array<float, KeyFloatCount> &candidateKey,
                  uint32_t candidateIndex)
    {
        valid = true;
        version = candidateVersion;
        key = candidateKey;
        index = candidateIndex;
    }

    void clear()
    {
        valid = false;
    }
};

template <size_t FloatCount>
inline uint32_t internFrameConstants(
    std::vector<std::array<float, FloatCount>> &values,
    std::unordered_map<uint64_t, std::vector<uint32_t>> &buckets,
    const std::array<float, FloatCount> &candidate)
{
    const uint8_t *bytes =
        reinterpret_cast<const uint8_t *>(candidate.data());
    const size_t byteCount = candidate.size() * sizeof(float);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < byteCount; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }

    std::vector<uint32_t> &bucket = buckets[hash];
    for (uint32_t index : bucket) {
        if (index < values.size() &&
            std::memcmp(values[index].data(), candidate.data(),
                        byteCount) == 0)
            return index;
    }

    const uint32_t index = static_cast<uint32_t>(values.size());
    values.push_back(candidate);
    bucket.push_back(index);
    return index;
}

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_FRAME_CONSTANTS_H */
