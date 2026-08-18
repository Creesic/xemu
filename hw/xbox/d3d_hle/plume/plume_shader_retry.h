#ifndef XGPU_PLUME_SHADER_RETRY_H
#define XGPU_PLUME_SHADER_RETRY_H

#include <cstdint>

namespace xgpu::plume {

/*
 * Suppress repeated compilation of the same deterministic failure while
 * retaining the ability to compile a replacement source. Runtime shader
 * compilation is lazy, so this state belongs to each registered shader.
 */
struct ShaderCompileRetryState {
    uint64_t sourceRevision = 1;
    uint64_t failedRevision = 0;

    bool shouldAttempt() const
    {
        return failedRevision != sourceRevision;
    }

    void noteFailure()
    {
        failedRevision = sourceRevision;
    }

    void noteSuccess()
    {
        failedRevision = 0;
    }

    void noteSourceChange()
    {
        ++sourceRevision;
        if (sourceRevision == 0)
            sourceRevision = 1;
        failedRevision = 0;
    }
};

} // namespace xgpu::plume

#endif /* XGPU_PLUME_SHADER_RETRY_H */
