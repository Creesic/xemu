#include "plume_span_replay.h"

void PlumeSpanReplayCache::store(uint32_t key, PlumeSpanPacket &&packet)
{
    m_packets[key] = std::move(packet);
}

const PlumeSpanPacket *PlumeSpanReplayCache::find(uint32_t key,
                                                  uint64_t currentFrame,
                                                  uint64_t maxAge) const
{
    auto it = m_packets.find(key);
    if (it == m_packets.end())
        return nullptr;
    if (currentFrame < it->second.frameStamp)
        return nullptr;
    if (currentFrame - it->second.frameStamp > maxAge)
        return nullptr;
    return &it->second;
}

void PlumeSpanReplayCache::erase(uint32_t key)
{
    m_packets.erase(key);
}

void PlumeSpanReplayCache::invalidateMask(uint32_t keyMask, uint32_t keyBits)
{
    for (auto it = m_packets.begin(); it != m_packets.end();) {
        if ((it->first & keyMask) == (keyBits & keyMask))
            it = m_packets.erase(it);
        else
            ++it;
    }
}

void PlumeSpanReplayCache::invalidateAll()
{
    m_packets.clear();
}

size_t PlumeSpanReplayCache::packetCount() const
{
    return m_packets.size();
}

void plume_span_replay_patch_camera(std::array<float, 768> &window,
                                    const float *cameraRows)
{
    std::memcpy(window.data() + 96u * 4u, cameraRows, 16u * sizeof(float));
}
