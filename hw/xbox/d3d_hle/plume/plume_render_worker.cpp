/* Stage-3 render worker: rendezvous execution of the render-owner job
 * bodies on a dedicated thread (see plume_render_worker.h). */

#include "plume_render_worker.h"
#include "plume_render_owner.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace xgpu {
namespace plume {

namespace {

enum class JobKind : uint8_t { None, WaitBatch, Present };

struct Job {
    JobKind kind = JobKind::None;
    uint32_t slot = 0;
    bool captureWaitSource = false;
    bool lazyDownloadFence = false;
    uint32_t waitDraws = 0;
    uint64_t waitDrawHash = 0;
    uint32_t presentReason = 0;
};

std::mutex g_mutex;
std::condition_variable g_jobReady;
std::condition_variable g_jobDone;
Job g_job;                       /* valid while g_jobPending */
bool g_jobPending = false;
/* Set by the owner body once every read of guest-latched or recorded
 * state is behind it; an async submit may return no earlier. */
bool g_inputsConsumed = false;
bool g_stop = false;
bool g_started = false;
std::atomic<bool> g_healthy{true};
std::thread g_thread;
thread_local bool g_on_worker_thread = false;

void worker_main()
{
    g_on_worker_thread = true;
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_jobReady.wait(lock,
                            [] { return g_jobPending || g_stop; });
            if (g_stop && !g_jobPending)
                return;
            job = g_job;
        }
        /* Owner bodies never throw today; a future failure path must set
         * ill health rather than tearing the process down from a worker. */
        if (job.kind == JobKind::WaitBatch) {
            xgpu_plume_owner_execute_wait_batch(
                job.slot, job.captureWaitSource, job.lazyDownloadFence,
                job.waitDraws, job.waitDrawHash);
        } else if (job.kind == JobKind::Present) {
            xgpu_plume_owner_execute_present(job.presentReason);
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_jobPending = false;
        }
        g_jobDone.notify_all();
    }
}

bool worker_start_locked()
{
    if (g_started)
        return g_healthy.load() && !g_stop;
    g_started = true;
    g_thread = std::thread(worker_main);
    std::atexit(plume_render_worker_stop);
    return true;
}

/* Hand the job over once any in-flight job has finished. When `wait` is
 * true this is a rendezvous; when false the job is left executing and the
 * next submit or plume_render_worker_sync() forms the completion barrier
 * (stage 4: at most one job — the present — runs ahead). */
bool run_job(const Job &job, bool wait)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_healthy.load() || g_stop || !worker_start_locked())
        return false;
    g_jobDone.wait(lock, [] { return !g_jobPending || g_stop; });
    if (g_stop)
        return false;
    g_job = job;
    g_jobPending = true;
    g_inputsConsumed = false;
    g_jobReady.notify_one();
    if (!wait) {
        /* Return once the owner body has consumed the recording and the
         * guest-latched inputs (or finished outright via an early skip),
         * so the guest may immediately record the next frame. */
        g_jobDone.wait(lock, [] {
            return g_inputsConsumed || !g_jobPending || g_stop;
        });
        return !g_stop;
    }
    g_jobDone.wait(lock, [] { return !g_jobPending || g_stop; });
    return !g_jobPending;
}

} /* namespace */

bool plume_render_worker_enabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("XRECOMP_PLUME_ASYNC_PRESENT");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool plume_render_worker_healthy()
{
    return g_healthy.load();
}

bool plume_render_worker_run_wait(uint32_t slot, bool capture_wait_source,
                                  bool lazy_download_fence,
                                  uint32_t wait_draws,
                                  uint64_t wait_draw_hash)
{
    if (!plume_render_worker_enabled())
        return false;
    Job job;
    job.kind = JobKind::WaitBatch;
    job.slot = slot;
    job.captureWaitSource = capture_wait_source;
    job.lazyDownloadFence = lazy_download_fence;
    job.waitDraws = wait_draws;
    job.waitDrawHash = wait_draw_hash;
    return run_job(job, /*wait=*/true);
}

bool plume_render_worker_run_present(uint32_t present_reason)
{
    if (!plume_render_worker_enabled())
        return false;
    Job job;
    job.kind = JobKind::Present;
    job.presentReason = present_reason;
    /* One present ahead: the guest continues recording the next frame
     * while the worker replays and presents this one. Every RHI-touching
     * or GPU-reading guest entry point synchronizes first. */
    return run_job(job, /*wait=*/false);
}

void plume_render_worker_mark_inputs_consumed()
{
    if (!g_on_worker_thread)
        return; /* inline placement: the caller already owns everything */
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_inputsConsumed = true;
    }
    g_jobDone.notify_all();
}

void plume_render_worker_sync()
{
    /* Owner-side code paths may re-enter shared helpers; the worker never
     * waits on itself. */
    if (!plume_render_worker_enabled() || g_on_worker_thread)
        return;
    std::unique_lock<std::mutex> lock(g_mutex);
    g_jobDone.wait(lock, [] { return !g_jobPending || g_stop; });
}

void plume_render_worker_stop()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_started || g_stop) {
            g_stop = true;
            g_healthy.store(false);
            return;
        }
        g_stop = true;
        g_healthy.store(false);
    }
    g_jobReady.notify_all();
    g_jobDone.notify_all();
    if (g_thread.joinable())
        g_thread.join();
}

} /* namespace plume */
} /* namespace xgpu */
