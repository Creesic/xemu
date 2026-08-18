#ifndef XGPU_PLUME_FRAME_DRAW_COUNTER_H
#define XGPU_PLUME_FRAME_DRAW_COUNTER_H

#include <cstddef>
#include <cstdint>
#include <limits>

namespace xgpu {
namespace plume {

class PlumeFrameDrawCounter {
public:
    void recordSubmission(size_t drawCount, bool frameBoundary)
    {
        const uint64_t total = static_cast<uint64_t>(m_currentFrameDraws) +
                               static_cast<uint64_t>(drawCount);
        m_currentFrameDraws =
            total > std::numeric_limits<uint32_t>::max()
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(total);
        if (frameBoundary) {
            m_previousFrameDraws = m_currentFrameDraws;
            m_currentFrameDraws = 0;
        }
    }

    uint32_t previousFrameDraws() const { return m_previousFrameDraws; }
    uint32_t currentFrameDraws() const { return m_currentFrameDraws; }

private:
    uint32_t m_currentFrameDraws = 0;
    uint32_t m_previousFrameDraws = 0;
};

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_FRAME_DRAW_COUNTER_H */
