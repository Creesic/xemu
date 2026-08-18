#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

/* One draw's slot in a span packet. The draw's opaque record lives in the
 * packet's contiguous geomBlobs at index * drawStride; offsets here are
 * rebased into the packet's own vertex/index slices; window indices point
 * into the packet's deduplicated constant-window tables. */
struct PlumeSpanDrawRef {
    uint32_t vertOffsetInPacket = 0;
    uint32_t vertByteLen = 0;
    uint32_t indexOffsetInPacket = 0; /* element offset into indexData */
    uint32_t indexCount = 0;          /* 0 = non-indexed */
    uint32_t vsWindowIndex = UINT32_MAX; /* UINT32_MAX = draw has no VS window */
    uint32_t psWindowIndex = UINT32_MAX;
};

struct PlumeSpanPacket {
    uint32_t drawStride = 0;          /* bytes per opaque draw record */
    std::vector<uint8_t> geomBlobs;   /* draws.size() * drawStride bytes */
    std::vector<PlumeSpanDrawRef> draws;
    std::vector<uint8_t> vertexBytes; /* one contiguous span slice */
    std::vector<uint32_t> indexData;  /* one contiguous span slice */
    /* Constant windows deduplicated by their source pool index at slice
     * time, so injection re-interns each unique window exactly once. */
    std::vector<std::array<float, 768>> vsWindows;
    std::vector<std::vector<float>> psWindows;
    uint64_t frameStamp = 0;
};

class PlumeSpanReplayCache {
public:
    void store(uint32_t key, PlumeSpanPacket &&packet);
    /* nullptr when missing or when currentFrame - frameStamp > maxAge.
     * find() never refreshes frameStamp: a packet recorded on frame N hits
     * on N+1 and N+2 and expires on N+3 (the 2/3 skip ceiling). */
    const PlumeSpanPacket *find(uint32_t key, uint64_t currentFrame,
                                uint64_t maxAge) const;
    void erase(uint32_t key);
    /* Drop every packet whose (key & keyMask) == (keyBits & keyMask). */
    void invalidateMask(uint32_t keyMask, uint32_t keyBits);
    void invalidateAll();
    size_t packetCount() const;
private:
    std::unordered_map<uint32_t, PlumeSpanPacket> m_packets;
};

/* Overwrite rows 96..99 (guest c[0..3], the world->screen camera composite)
 * of a cached 192-float4 window with the 16 floats at cameraRows — sourced
 * at inject time from the donor draw's interned VS window. */
void plume_span_replay_patch_camera(std::array<float, 768> &window,
                                    const float *cameraRows);
