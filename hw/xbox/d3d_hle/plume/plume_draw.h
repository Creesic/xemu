/*
 * plume_draw.h — Draw replay, PSO cache, texture mirroring, and xps shader path.
 *
 * Records per-frame DrawPrimitiveUP geometry from the D3D8 hook layer and
 * replays it into a Plume command list owned by PlumeContext.
 */
#ifndef XGPU_PLUME_DRAW_H
#define XGPU_PLUME_DRAW_H

#include <array>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace xgpu {
namespace plume {
class PlumeContext;
} /* namespace plume */
} /* namespace xgpu */

#include "plume_render_interface.h"
#include "plume_host.h"
#include "plume_frame_constants.h"
#include "plume_frame_draw_counter.h"
#include "plume_recorded_resources.h"
#include "plume_shader_compiler.h"
#include "plume_shader_retry.h"
#include "plume_mesh_cache.h"
#include "plume_span_replay.h"
#include "plume_surface_binding.h"

namespace xgpu {
namespace plume {

class PlumeDraw {
public:
    void reset();
    /* Configure the initial logical output and integer physical surface scale
     * before initPipelines(). Guest-facing extents remain logical. */
    bool configureRenderExtent(uint32_t logicalWidth, uint32_t logicalHeight,
                               uint32_t internalResolutionScale);
    /* Total-order renderer-owner transition. The caller must first submit and
     * retire every recorded/in-flight batch. Existing colour surfaces are
     * filtered into replacement physical allocations; depth is recreated. */
    bool reconfigureRenderExtent(PlumeContext &ctx,
                                 uint32_t logicalWidth,
                                 uint32_t logicalHeight,
                                 uint32_t internalResolutionScale);
    uint32_t outputWidth() const { return m_outputWidth; }
    uint32_t outputHeight() const { return m_outputHeight; }
    uint32_t internalResolutionScale() const {
        return m_internalResolutionScale;
    }
    uint32_t physicalOutputWidth() const;
    uint32_t physicalOutputHeight() const;

    bool initPipelines(PlumeContext &ctx);
    bool ready() const { return m_pipelinesReady; }

    void setTexture(uint32_t stage, uint32_t guest,
                    const void *pixels, uint32_t w, uint32_t h, uint32_t pitch,
                    uint32_t bytes, uint32_t format, uint64_t version,
                    uint32_t depth = 1, uint32_t levels = 1,
                    uint32_t dimensionality = 2, uint32_t cube = 0,
                    uint32_t unnormalizedCoords = 0);
    bool bindTextureIfCached(const XgpuTextureBinding &binding);
    /* Guest recorder: latch only the Xbox sampler description. The render
     * owner resolves/creates the matching RHI sampler during replay. */
    void setSampler(const XgpuSamplerBinding &binding);
    bool setRenderTarget(const XgpuSurfaceBinding &binding);
    void clearTarget(float r, float g, float b, float a,
                     uint32_t colorWriteMask,
                     const XgpuRect *rect);
    void clearDepthStencil(bool clearDepth, bool clearStencil,
                           float depth, uint8_t stencil,
                           const XgpuRect *rect);
    bool setSurfaceTexture(uint32_t stage, uint32_t guest,
                           uint32_t unnormalizedCoords);
    bool blitSurface(PlumeContext &ctx, uint32_t dstGuest, uint32_t srcGuest,
                     uint32_t width, uint32_t height);
    bool ensureColorSurface(PlumeContext &ctx, uint64_t generation,
                            uint32_t width, uint32_t height);
    bool downloadColorSurface(PlumeContext &ctx, uint32_t guest, void *pixels,
                              uint32_t width, uint32_t height,
                              uint32_t pitch);
    bool uploadColorSurface(PlumeContext &ctx, uint32_t guest,
                            const void *pixels, uint32_t width,
                            uint32_t height, uint32_t pitch);
    /* Resolve a rendered DEPTH surface back to guest memory as packed Z24S8.
     * Needed because a guest texture may alias the depth buffer rather than a
     * colour surface; resolving colour alone leaves such a stage reading zero. */
    bool zetaSurfaceNeedsDownload(uint32_t guest, uint32_t width,
                                  uint32_t height, uint32_t pitch) const;
    bool downloadZetaSurface(PlumeContext &ctx, uint32_t guest, void *pixels,
                             uint32_t width, uint32_t height, uint32_t pitch);
    /* Render owner: advance content serials while consuming recorded writes;
     * pairs with the CPU download deduplication paths. */
    void noteZetaWrite(uint64_t zetaGeneration);
    void noteColorWrite(uint64_t colorGeneration);
    /* Render owner: satisfy a pitch-linear Y16 view of the live zeta at
     * `zetaGuest` from an on-GPU conversion (no CPU round trip). Returns
     * false whenever anything cannot be proven (unknown zeta, dimension
     * mismatch, pipeline failure) so the caller falls back to the legacy
     * resolve+upload path. */
    bool bindZetaAliasStage(PlumeContext &ctx, uint32_t stage,
                            uint32_t zetaGuest, uint32_t aliasWidth,
                            uint32_t aliasHeight, uint32_t aliasPitch,
                            uint32_t unnormalizedCoords);
    /* True when recorded or in-flight color work could leave guest memory
     * stale for this surface, so a CPU lock must drain the device first.
     * False only when the WAIT-download pipeline has already packed every
     * recorded write back into guest RAM. Unknown surfaces report pending so
     * callers preserve their synchronization. */
    bool colorSurfacePendingCpuSync(uint32_t guest) const;
    bool hasSurface(uint32_t guest) const {
        return m_guestLatestSurfaceGeneration.find(guest) !=
               m_guestLatestSurfaceGeneration.end();
    }
    /* Mint the on-demand device-backbuffer mirror surface so present-target
     * aliases always bind the surface path (refreshed from the screen at
     * replay); no-op unless guest is the present target. */
    bool ensureBackbufferMirror(PlumeContext &ctx, uint32_t guest);
    void setPresentSurface(uint32_t guest);
    void recordDraw(PlumeContext &ctx, uint32_t primType, uint32_t primCount,
                    const void *verts, uint32_t stride, uint32_t fvf,
                    const XgpuPlumeRenderState *renderState);
    bool recordIndexedDraw(
        PlumeContext &ctx, uint32_t primType, uint32_t primCount,
        const void *verts, uint32_t vertexCount,
        uint32_t stride, uint32_t fvf,
        const uint32_t *indices, uint32_t indexCount,
        const XgpuPlumeRenderState *renderState);
    bool recordCachedIndexedDraw(PlumeContext &ctx,
                                 const XgpuPlumeCachedIndexedDraw &draw);
    void meshCacheInvalidateVa(uint32_t resource_data_va);
    bool queueHostFrame(PlumeContext &ctx, const void *pixels, uint32_t w,
                        uint32_t h, uint32_t pitch, uint32_t format,
                        uint64_t version);
    bool queueHostOverlay(PlumeContext &ctx,
                          const XgpuPlumeDebugOverlayFrame &frame,
                          uint32_t slot);
    bool queueHostOutputOverlay(PlumeContext &ctx,
                                const XgpuPlumeDebugOverlayFrame &frame,
                                uint32_t slot);
    /* Re-blit the last host frame when the game presents with no draws
     * (FMV path: host player paints, then guest Present would otherwise clear). */
    void ensureStickyHostFrame(PlumeContext &ctx);
    /* Scale a native Xbox scanout surface into a wider host output. This is
     * recorded after guest draws and before host debug overlays. */
    void ensurePresentSurfaceComposite(PlumeContext &ctx);
    bool hasQueuedWork() const { return queuedCount() != 0; }
    size_t queuedCount() const {
        return m_rec.draws.size() + m_rec.progDraws.size();
    }
    bool needsStickyHostFrame() const {
        return m_stickyHostFrame && !hasQueuedWork();
    }
    bool hasStickyHostFrame() const { return m_stickyHostFrame; }
    bool retireStickyHostFrame() {
        const bool retired = m_stickyHostFrame;
        m_stickyHostFrame = false;
        return retired;
    }
    uint32_t presentGuest() const { return m_presentTarget; }
    uint32_t drawsToPresentSurface() const;
    ::plume::RenderTexture *resolvedPresentSurface(uint32_t *width,
                                                   uint32_t *height,
                                                   ::plume::RenderTextureLayout *layout = nullptr) const {
        auto latest = m_latestSurfaceGeneration.find(m_presentTarget);
        auto surface = latest != m_latestSurfaceGeneration.end()
            ? m_surfaceCache.find(latest->second) : m_surfaceCache.end();
        if (!m_presentTarget || surface == m_surfaceCache.end())
            return nullptr;
        if (width)
            *width = surface->second.width;
        if (height)
            *height = surface->second.height;
        if (layout)
            *layout = surface->second.layout;
        return surface->second.texture.get();
    }
    void replay(PlumeContext &ctx, ::plume::RenderCommandList *cmdList,
                ::plume::RenderTexture *screenTexture,
                ::plume::RenderFramebuffer *screenFramebuffer,
                bool copyPresentSurface, bool frameBoundary);
    /* Filter the physical scene into the current host swapchain target. */
    bool recordOutputScale(PlumeContext &ctx,
                           ::plume::RenderCommandList *cmdList,
                           ::plume::RenderTexture *source,
                           ::plume::RenderTextureView *sourceView,
                           ::plume::RenderFramebuffer *destinationFramebuffer,
                           uint32_t destinationWidth,
                           uint32_t destinationHeight);
    bool recordHostOutputOverlays(
        PlumeContext &ctx,
        ::plume::RenderCommandList *cmdList,
        ::plume::RenderFramebuffer *destinationFramebuffer,
        uint32_t destinationWidth,
        uint32_t destinationHeight);
    void releaseSubmittedResources();
    /* Pipelined-present retire: release only the output-scale descriptor
     * sets, which are referenced solely by the just-retired present
     * submission. The programmable descriptor cache and retired textures
     * stay live until a total-order sync point (a descriptor-batch flush or
     * a synchronous present), because in-flight WAIT batches may still
     * reference them. */
    void releaseOutputScaleDescriptors();
    void setPipelinedPresent(bool enabled) { m_pipelinedPresent = enabled; }
    /* Small-surface CPU download: record copy commands for every small
     * color surface written by the replay just recorded on `cmdList`
     * (call before end()), then after the submission's fence completes,
     * write the pixels back to guest RAM (Xbox VRAM is plain RAM). */
    void recordSurfaceDownloads(PlumeContext &ctx,
                                ::plume::RenderCommandList *cmdList);
    void completeSurfaceDownloads();
    /* Pipelined present: move the just-recorded present-CL downloads onto a
     * dedicated deferred list so intervening WAIT batches neither steal them
     * (retireSub) nor eager-fence on their account. Completed at the next
     * present's retire, one frame later — safe for the only known per-frame
     * consumer, MM3's exposure meter, which reads its 4-deep 40x30 luminance
     * ring at distance 3 frames into a 0.15/frame exponential filter
     * (mm3_lum_exposure_update @ guest 0x2305D1). */
    void deferSurfaceDownloads();
    void completeDeferredSurfaceDownloads();

    /* Async WAIT_FOR_IDLE submission ring. MM3 issues ~20 WAIT_FOR_IDLEs per
     * frame; blocking on each one's GPU fence cost most of the in-game frame
     * time. Slots 0..kWaitRingSize-1 are the async in-flight WAIT batches;
     * kPresentSub is the dedicated (synchronous) present submission. Each slot
     * owns its own vertex/const upload buffers so the CPU never overwrites data
     * the GPU is still reading, and retains its descriptors/textures/downloads
     * until its fence signals. */
    static constexpr uint32_t kWaitRingSize = 3;
    static constexpr uint32_t kPresentSub = kWaitRingSize;
    /* Select which submission's upload buffers the next replay() fills. */
    void setCurrentSub(uint32_t idx) {
        m_curSub = (idx <= kPresentSub) ? idx : kPresentSub;
    }
    /* Hand the just-submitted batch's in-flight descriptors/textures/downloads
     * to slot idx so they outlive its GPU work (freed by reclaimSub). */
    void retireSub(uint32_t idx);
    /* Slot idx's fence has signaled: flush its queued guest-RAM downloads and
     * free its retained descriptors/textures. */
    void reclaimSub(uint32_t idx);
    /* True when the just-recorded replay queued small-surface CPU downloads; the
     * caller must fence before the guest may read them back. */
    bool pendingDownloads() const { return !m_pendingDownloads.empty(); }

    uint32_t createPixelShader(const char *text);
    void setActivePS(uint32_t handle);
    void setPSConst(uint32_t start, const float *values, uint32_t count);
    void setCombinerConsts(const float *values); /* 16 float4 */
    void setCombinerConstsEx(const float *values, uint32_t float4Count);
    bool registerVertexShader(uint32_t handle, const char *hlsl,
                              uint16_t inputsRead,
                              uint16_t outputsWritten,
                              const XgpuPlumeVertexDeclaration *declaration);
    void setActiveVertexShader(uint32_t handle);
    void setVertexShaderConstants(const float *values,
                                  uint32_t float4Count);
    void setVertexData4f(uint32_t reg, const float *value);

    /* F2 metadata sampled before replay(), which clears the draw queue. */
    struct F2QueueFingerprint {
        uint32_t draws_to_present = 0;
        uint64_t draw_hash = 0;
    };
    F2QueueFingerprint captureF2QueueFingerprint() const;

private:
    struct GeomDraw {
        uint32_t offset, byteLen, vertexCount, stride;
        uint32_t indexOffset, indexCount; /* uint32 indices; zero = nonindexed */
        uint32_t cachedMeshId; /* 0 = frame stream; else persistent GPU mesh */
        uint32_t fvf;
        uint32_t activePsHandle;
        uint8_t topology, hasDiffuse, hasSpecular, uvOffset;
        uint32_t psHandle;
        uint32_t progConstIndex;
        uint32_t vsHandle;
        uint32_t vsConstIndex;
        uint8_t hasUV;
        uint8_t texCount;
        /* Host diagnostics composite after the guest present-surface copy. */
        uint8_t afterPresentCopy;
        uint32_t targetGuest, targetWidth, targetHeight;
        uint64_t targetColorGeneration;
        uint64_t targetZetaGeneration;
        uint64_t targetFramebufferGeneration;
        RecordedTextureBinding stageTexture[4];
        XgpuSamplerBinding stageSamplerState[4];
        uint8_t stageSamplerStateValid[4];
        uint64_t surfaceStage[4];
        uint8_t clear;
        uint8_t clearDepth;
        uint8_t clearStencil;
        /* Guest-recorded intent; replay advances render-owned serials. */
        uint8_t recordsColorWrite;
        uint8_t recordsZetaWrite;
        float clearColor[4];
        float depthClear;
        uint8_t stencilClear;
        uint8_t hasClearRect;
        XgpuRect clearRect;
        XgpuPlumeRenderState renderState;
        /* Nonzero: this op is a 2D-engine surface blit, not a draw. */
        uint64_t blitSrcGeneration;
        uint64_t blitDstGeneration;
    };

    struct PlumeTex {
        std::unique_ptr<::plume::RenderTexture> tex;
        std::unique_ptr<::plume::RenderTextureView> view;
        std::unique_ptr<::plume::RenderDescriptorSet> descSet;
        std::unique_ptr<::plume::RenderBuffer> hostUpload;
        size_t hostUploadBytes = 0;
        std::vector<uint8_t> hostConverted;
        uint32_t w = 0, h = 0, d = 1, levels = 1, dimension = 2;
        uint32_t fmt = 0, bytes = 0, cube = 0;
        uint32_t unnormalizedCoords = 0;
        uint64_t version = 0;
    };

    struct PlumePixelShader {
        std::string hlsl;
        std::string diagnostics;
        std::vector<uint8_t> bytecode;
        std::string entryPoint = "main";
        ShaderTarget target = ShaderTarget::DXIL;
        bool ok = false;
        /* Shader reads combiner constants from the per-draw CB (b8)
         * instead of baked literals; snapshot m_combinerConst at record. */
        bool combinerCB = false;
        ShaderCompileRetryState compileRetry;
        std::future<ShaderCompileResult> compileFuture;
        std::unique_ptr<::plume::RenderShader> shader;
        /* Opt-in live HLSL override. The runtime seeds this path with the
         * generated shader, then recompiles only after its timestamp changes. */
        std::string livePath;
        int64_t liveWriteStamp = 0;
    };

    struct PlumeVertexShader {
        std::string hlsl;
        std::string diagnostics;
        std::vector<uint8_t> bytecode;
        std::string entryPoint = "main";
        ShaderTarget target = ShaderTarget::DXIL;
        uint16_t inputsRead = 0;
        uint16_t outputsWritten = 0;
        uint16_t attributesPresent = 0;
        std::array<uint8_t, XGPU_PLUME_VERTEX_ATTRIBUTE_COUNT> stream = {};
        std::array<uint8_t, XGPU_PLUME_VERTEX_ATTRIBUTE_COUNT> format = {};
        std::array<uint16_t, XGPU_PLUME_VERTEX_ATTRIBUTE_COUNT> offset = {};
        bool hasDeclaration = false;
        bool ok = false;
        std::future<ShaderCompileResult> compileFuture;
        std::unique_ptr<::plume::RenderShader> shader;
    };

    struct PlumeColorSurface {
        std::unique_ptr<::plume::RenderTexture> texture;
        std::unique_ptr<::plume::RenderTextureView> view;
        std::unique_ptr<::plume::RenderDescriptorSet> descSet;
        std::unique_ptr<::plume::RenderTexture> snapshot;
        std::unique_ptr<::plume::RenderTextureView> snapshotView;
        std::unique_ptr<::plume::RenderDescriptorSet> snapshotDescSet;
        uint32_t width = 0, height = 0;
        uint32_t physicalWidth = 0, physicalHeight = 0;
        /* Guest identity for CPU download (Xbox VRAM is plain RAM: the
         * game CPU-reads small render targets, e.g. its 40x30 exposure
         * measurement ring). Zero when unknown / not downloadable. */
        uint32_t guestAddr = 0;
        uint32_t guestPitch = 0;
        uint32_t guestFormat = 0;
        std::unique_ptr<::plume::RenderBuffer> downloadBuffer;
        ::plume::RenderTextureLayout layout = ::plume::RenderTextureLayout::UNKNOWN;
        ::plume::RenderTextureLayout snapshotLayout = ::plume::RenderTextureLayout::UNKNOWN;
        /* Color-content dirtiness for CPU-lock drain dedup, mirroring the
         * zeta serials. contentSerial advances when the render owner consumes
         * a recorded color write; resolvedSerial advances when the
         * small-surface WAIT download packs those bytes into guest memory (or
         * an upload makes host and guest content identical). Equal serials
         * prove guest memory already holds this surface's pixels, so a CPU
         * lock needs no device-wide drain. */
        uint64_t contentSerial = 1;
        uint64_t resolvedSerial = 0;
    };

    struct PlumeZetaSurface {
        std::unique_ptr<::plume::RenderTexture> texture;
        ::plume::RenderFormat format = ::plume::RenderFormat::UNKNOWN;
        ::plume::RenderTextureLayout layout = ::plume::RenderTextureLayout::UNKNOWN;
        uint32_t width = 0, height = 0;
        uint32_t physicalWidth = 0, physicalHeight = 0;
        /* Guest identity, so a texture that aliases this address can have the
         * depth resolved back to guest memory. MM3 samples its Z24S8 buffer as
         * a Y16 luminance texture to build the destination-alpha sky mask. */
        uint32_t guestAddr = 0;
        uint32_t guestPitch = 0;
        /* Depth-content dirtiness for download dedup. contentSerial advances
         * when the render owner consumes a recorded depth write or clear;
         * downloadZetaSurface records the serial it resolved. Equal
         * serials mean guest memory already holds these depth bytes, so the
         * readback stall and the caller's re-upload can both be skipped
         * (MM3 binds its sky-mask Y16 alias 3x per frame). */
        uint64_t contentSerial = 1;
        uint64_t downloadedSerial = 0;
        /* GPU alias conversion state (plume-gpu-zeta-alias-conversion.md):
         * serial of the last on-GPU Y16 conversion, plus the reusable
         * R32_FLOAT staging chain (depth plane 0 is copied through a
         * buffer because sampling the depth image directly is not
         * guaranteed across backends). */
        uint64_t convertedSerial = 0;
        std::unique_ptr<::plume::RenderBuffer> convertScratch;
        std::unique_ptr<::plume::RenderTexture> convertSource;
    };

    struct PlumeFramebuffer {
        std::unique_ptr<::plume::RenderFramebuffer> framebuffer;
        uint64_t colorGeneration = 0;
        uint64_t zetaGeneration = 0;
    };

    ::plume::RenderPipeline *geomPso(PlumeContext &ctx, uint32_t stride,
                                     uint8_t topology, uint8_t hasDiffuse,
                                     const XgpuPlumeRenderState &renderState,
                                     bool hasDepthAttachment);
    ::plume::RenderPipeline *texPso(PlumeContext &ctx, uint32_t stride,
                                    uint8_t topology, uint8_t hasDiffuse,
                                    uint8_t uvOffset,
                                    const XgpuPlumeRenderState &renderState,
                                    bool hasDepthAttachment);
    struct ProgPsoFailure {
        const char *reason = "none";
        uint32_t attr = 0xFFFFFFFFu;
        uint32_t stream = 0;
        uint32_t format = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
    };
    ::plume::RenderPipeline *progPso(PlumeContext &ctx, uint32_t psHandle,
                                      uint32_t vsHandle,
                                      uint32_t stride, uint8_t topology,
                                      uint8_t hasDiffuse, uint8_t hasSpecular,
                                      uint8_t hasUV,
                                       uint8_t texCount,
                                       uint8_t uvOffset, uint32_t fvf,
                                       const XgpuPlumeRenderState &renderState,
                                       bool hasDepthAttachment,
                                       ProgPsoFailure *failure);
    /* Builds/caches the texture+sampler descriptor set for a draw's four stages.
     * Templated so both GeomDraw and ProgDraw (identical stage fields) reuse it. */
    template <class Draw>
    ::plume::RenderDescriptorSet *createProgDrawDescriptorSet(
        PlumeContext &ctx, const Draw &draw,
        const ::plume::RenderBuffer *constantBuffer,
        uint64_t constantOffset,
        const ::plume::RenderBuffer *vertexConstantBuffer,
        uint64_t vertexConstantBufferSize);
    ::plume::RenderShader *progPixelShader(PlumeContext &ctx, uint32_t handle);
    ::plume::RenderShader *progVertexShader(PlumeContext &ctx,
                                            uint32_t handle);
    void pollPixelShaderOverrides(PlumeContext &ctx);
    uint64_t normalizeSampledSurfaceGeneration(
        uint64_t sampledGeneration, uint64_t targetGeneration) const;
    /* One queued small-surface readback: the color generation plus the
     * content serial its copy command observes, credited to resolvedSerial
     * once the pixels land in guest RAM. */
    struct PendingSurfaceDownload {
        uint64_t generation;
        uint64_t contentSerial;
    };

    /* Map the readback buffers for `downloads` and copy their pixels into
     * guest RAM. */
    void completeDownloadsFrom(
        const std::vector<PendingSurfaceDownload> &downloads);

    bool m_pipelinesReady = false;
    uint32_t m_outputWidth = XGPU_PANEL_WIDTH;
    uint32_t m_outputHeight = XGPU_PANEL_HEIGHT;
    uint32_t m_internalResolutionScale = 1;

    bool m_outputScaleReady = false;
    std::unique_ptr<::plume::RenderShader> m_outputScaleVS;
    std::unique_ptr<::plume::RenderShader> m_outputScalePS;
    std::unique_ptr<::plume::RenderSampler> m_outputScaleSampler;
    std::unique_ptr<::plume::RenderPipelineLayout> m_outputScaleLayout;
    std::unique_ptr<::plume::RenderPipeline> m_outputScalePso;
    std::unique_ptr<::plume::RenderPipeline> m_outputOverlayPso;
    bool ensureZetaAliasPipeline(PlumeContext &ctx);
    bool m_zetaAliasReady = false;
    std::unique_ptr<::plume::RenderShader> m_zetaAliasVS;
    std::unique_ptr<::plume::RenderShader> m_zetaAliasPS;
    std::unique_ptr<::plume::RenderSampler> m_zetaAliasSampler;
    std::unique_ptr<::plume::RenderPipelineLayout> m_zetaAliasLayout;
    std::unique_ptr<::plume::RenderPipeline> m_zetaAliasPso;
    std::vector<std::unique_ptr<::plume::RenderDescriptorSet>>
        m_outputScaleDescriptors;

    struct HostOutputOverlay {
        RecordedTextureBinding binding;
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    std::vector<HostOutputOverlay> m_hostOutputOverlays;

    bool m_geomReady = false;
    std::unique_ptr<::plume::RenderShader> m_geomVS;
    std::unique_ptr<::plume::RenderShader> m_geomVSPlain;
    std::unique_ptr<::plume::RenderShader> m_geomPS;
    std::unique_ptr<::plume::RenderPipelineLayout> m_geomLayout;
    std::unordered_map<uint64_t, std::unique_ptr<::plume::RenderPipeline>> m_geomPsos;

    /* One in-flight submission's volatile GPU-read resources. vbuf/progCB are
     * mapped and read during replay, so every concurrently-submitted batch needs
     * its own; the buckets retain descriptors/replaced-textures/download gens
     * until that batch's fence signals (see retireSub/reclaimSub). */
    struct Submission {
        std::unique_ptr<::plume::RenderBuffer> vbuf;
        size_t vbufCap = 0;
        std::unique_ptr<::plume::RenderBuffer> progCB;
        size_t progCBCap = 0;
        /* GPU vertex-transform (indexed programmable) path: raw vertex data,
         * indices, and the 192-float4 VS constant buffer. */
        std::unique_ptr<::plume::RenderBuffer> rawVbuf;
        size_t rawVbufCap = 0;
        std::unique_ptr<::plume::RenderBuffer> ibuf;
        size_t ibufCap = 0;
        std::unique_ptr<::plume::RenderBuffer> vsCB;
        size_t vsCBCap = 0;
        std::vector<std::unique_ptr<::plume::RenderDescriptorSet>> descBucket;
        std::vector<PlumeTex> retiredBucket;
        std::vector<PendingSurfaceDownload> downloadBucket;
    };
    Submission m_sub[kWaitRingSize + 1];
    uint32_t m_curSub = kPresentSub;
    bool m_pipelinedPresent = false;
    std::vector<uint64_t> m_replayWritten;   /* color gens written by replay */
    std::vector<PendingSurfaceDownload> m_pendingDownloads; /* queued readbacks */
    std::vector<PendingSurfaceDownload> m_deferredPresentDownloads;

    /* Deferred indexed programmable draw: raw vertex attributes fetched and
     * transformed on the GPU by the translated NV2A vertex shader, replacing the
     * per-vertex CPU interpreter. One shared raw-vertex staging buffer / index
     * staging / VS-constant staging per frame batch, mirroring the geometric
     * frame-vertex stream. */
    struct ProgDraw {
        uint32_t vbufOffset, vbufBytes, stride;   /* raw interleaved vertices */
        uint32_t ibufOffset, indexCount;          /* uint32 indices */
        uint32_t vsConstIndex;                    /* 192-float4 constant slot */
        uint32_t progConstIndex;                  /* combiner PS b8 CB slot */
        float viewportZOffset, viewportZScale;    /* recorded Xbox viewport */
        uint16_t attrsUsed;                       /* v-register bitmask */
        uint32_t attrFormat[16];                  /* NV2A format per used attr */
        uint32_t attrOffset[16];                  /* byte offset within a vertex */
        uint8_t topology;
        uint32_t psHandle;
        uint64_t vertexProgramKey;
        uint32_t targetGuest, targetWidth, targetHeight;
        uint64_t targetColorGeneration, targetZetaGeneration,
                 targetFramebufferGeneration;
        RecordedTextureBinding stageTexture[4];
        XgpuSamplerBinding stageSamplerState[4];
        uint8_t stageSamplerStateValid[4];
        uint64_t surfaceStage[4];
        uint8_t texCount;
        uint8_t recordsColorWrite;
        uint8_t recordsZetaWrite;
        XgpuPlumeRenderState renderState;
        /* Number of GeomDraws queued before this one; replay draws this ProgDraw
         * just before GeomDraw[afterGeom] to preserve submission order. */
        uint32_t afterGeom;
    };

public:
    struct SurfaceBindingCommand {
        XgpuSurfaceBinding binding = {};
        PlumeSurfaceBindingIds ids = {};
    };

    struct VertexProgramCommand {
        uint64_t key = 0;
        uint16_t inputsRead = 0;
        ShaderTarget target = ShaderTarget::DXIL;
        std::string entryPoint;
        std::vector<uint8_t> bytecode;
    };

    /* Replay-input state for one recorded frame. Grouping it into one publicly
     * nameable, owned value establishes the later handoff seam; it is not yet
     * a complete cross-thread packet because cache/resource ownership remains
     * in PlumeDraw. */
    struct FrameRecording {
        /* Scanout selection is frame-owned. Xbox titles may alternate two
         * guest frame buffers at each Swap while the owner still completes
         * the prior host present, so preserve both address and generation. */
        uint32_t presentTarget = 0;
        uint64_t presentGeneration = 0;
        std::vector<GeomDraw> draws;
        std::vector<ProgDraw> progDraws;
        std::vector<uint8_t> frameVerts;
        std::vector<uint8_t> frameRawVerts;
        std::vector<uint32_t> frameIndices;
        std::vector<std::array<float, 768>> frameVSConsts;
        std::unordered_map<uint64_t, std::vector<uint32_t>>
            frameVSConstBuckets;
        /* Reuse the most recent vertex-constant snapshot while neither the
         * source constants nor their derived viewport rows have changed. */
        FrameConstantVersionCache<8> frameVSConstCache;
        /* The first 21 float4s cover the shared combiner/fixed/XPS prefix,
         * including the table-fog parameters in row 20. Rows 21..24 carry
         * per-texture coordinate scale for Xbox linear textures. */
        std::vector<std::array<float, 100>> frameProgConsts;
        std::unordered_map<uint64_t, std::vector<uint32_t>>
            frameProgConstBuckets;
        /* Reuse the most recent pixel-constant snapshot while both its
         * source version and derived per-texture coordinate scales match. */
        FrameConstantVersionCache<16> frameProgConstCache;
        /* Owned resource commands captured while recording and consumed by
         * the render owner before replay resolves any referenced resources. */
        std::vector<RecordedTextureUpload> textureUploads;
        std::vector<SurfaceBindingCommand> surfaceBindings;
        std::vector<VertexProgramCommand> vertexPrograms;

        void clear()
        {
            presentTarget = 0;
            presentGeneration = 0;
            draws.clear();
            progDraws.clear();
            frameVerts.clear();
            frameRawVerts.clear();
            frameIndices.clear();
            frameVSConsts.clear();
            frameVSConstBuckets.clear();
            frameVSConstCache.clear();
            frameProgConsts.clear();
            frameProgConstBuckets.clear();
            frameProgConstCache.clear();
            textureUploads.clear();
            surfaceBindings.clear();
            vertexPrograms.clear();
        }
    };

    FrameRecording takeRecording();
    /* Render owner: create/upload every texture payload the recording
     * carries and publish them into the versioned store, before the
     * recording's draws are replayed. */
    void consumeTextureUploads(PlumeContext &ctx, FrameRecording &recording,
                               ::plume::RenderCommandList *cmdList);
    void consumeSurfaceBindings(PlumeContext &ctx, FrameRecording &recording);
    void consumeVertexPrograms(PlumeContext &ctx, FrameRecording &recording);
    /* Owner boundary used before present-only composites inspect the surface
     * cache; replay calls the same consumer as a no-op fallback. */
    void materializeRecordedSurfaces(PlumeContext &ctx) {
        consumeSurfaceBindings(ctx, m_rec);
    }
    void replayRecording(PlumeContext &ctx, FrameRecording &&recording,
                         ::plume::RenderCommandList *cmdList,
                         ::plume::RenderTexture *screenTexture,
                         ::plume::RenderFramebuffer *screenFramebuffer,
                         bool copyPresentSurface, bool frameBoundary);

    /* Named recording spans over the frame recording. A span brackets the
     * draws one caller-defined unit appends; a cached span can later be
     * re-injected in place of re-recording it. Keys are opaque. */
    void spanBegin(uint32_t key);
    uint32_t spanEnd(uint32_t key);
    bool spanTryReplay(uint32_t key);
    void spanInvalidate(uint32_t key);
    void spanInvalidateMask(uint32_t keyMask, uint32_t keyBits);
    void spanInvalidateAll();
    /* Number of GeomDraws recorded so far in the current frame recording. */
    uint32_t recordedDrawCount() const
    {
        return static_cast<uint32_t>(m_rec.draws.size());
    }

private:
    struct RecordSpan {
        bool active = false;
        uint32_t key = 0;
        size_t drawBase = 0;
        size_t progBase = 0;
        size_t vertBase = 0;
        size_t indexBase = 0;
    };
    RecordSpan m_recordSpan;
    /* Open-span content accumulated across mid-pass flushes: takeRecording
     * slices the live pools into this partial packet before they are moved,
     * then re-arms the span bases at zero. */
    PlumeSpanPacket m_spanPartial;
    bool m_spanRejected = false;
    void spanSliceInto(PlumeSpanPacket &packet);
    PlumeSpanReplayCache m_spanReplay;
    uint64_t m_frameIndex = 0;
    static constexpr uint64_t kSpanReplayMaxAgeFrames = 2;
    bool m_omitVertexBytes = false;
    struct CachedMeshGpu {
        std::unique_ptr<::plume::RenderBuffer> vb;
        std::unique_ptr<::plume::RenderBuffer> ib;
        uint32_t vbBytes = 0;
        uint32_t indexCount = 0;
        uint32_t stride = 0;
    };
    std::vector<CachedMeshGpu> m_cachedMeshes;
    uint32_t m_cachedHits = 0;
    uint32_t m_cachedMisses = 0;
    uint32_t m_cachedFallbacks = 0;
    uint64_t m_cachedBytesSaved = 0;

    FrameRecording m_rec;
    std::unique_ptr<::plume::RenderPipelineLayout> m_progIdxLayout;
    std::unordered_map<uint64_t, std::unique_ptr<::plume::RenderPipeline>> m_progIdxPsos;

    bool m_texReady = false;
    RecordedTextureVersions<PlumeTex> m_textures;
    PlumeSurfaceBindingTracker m_surfaceBindingTracker;
    std::unordered_map<uint64_t, PlumeColorSurface> m_surfaceCache;
    std::unordered_map<uint64_t, PlumeZetaSurface> m_zetaCache;
    std::unordered_map<uint64_t, PlumeFramebuffer> m_framebufferCache;
    std::unordered_map<uint32_t, uint64_t> m_latestSurfaceGeneration;
    /* Guest address -> zeta generation, mirroring m_latestSurfaceGeneration. */
    std::unordered_map<uint32_t, uint64_t> m_latestZetaGeneration;
    /* Guest recorder shadows. The render owner never reads or mutates these;
     * they make surface binding and alias normalization value-only even when
     * the corresponding RHI resource command has not executed yet. */
    std::unordered_map<uint32_t, uint64_t> m_guestLatestSurfaceGeneration;
    std::unordered_map<uint32_t, uint64_t> m_guestLatestZetaGeneration;
    std::unordered_map<uint64_t, uint32_t> m_guestSurfaceAddress;
    std::unique_ptr<::plume::RenderSampler> m_texSampler;
    std::unique_ptr<::plume::RenderPipelineLayout> m_texLayout;
    std::unique_ptr<::plume::RenderShader> m_texVS;
    std::unique_ptr<::plume::RenderShader> m_texVSDiffuse;
    std::unique_ptr<::plume::RenderShader> m_texPS;
    std::unordered_map<uint64_t, std::unique_ptr<::plume::RenderPipeline>> m_texPsos;
    RecordedTextureBinding m_curTextureStage[4] = {};
    /* Guest-side shadow of the last recorded upload per guest address.
     * The probe (bindTextureIfCached) answers from this without touching
     * the render-owned versioned store, so the whole texture record path
     * needs no completion barrier. */
    std::unordered_map<uint32_t, RecordedTextureBinding> m_guestTextureShadow;
    void uploadRecordedTexture(PlumeContext &ctx,
                               RecordedTextureUpload &&upload,
                               ::plume::RenderCommandList *cmdList);
    bool applySurfaceBinding(
        PlumeContext &ctx,
        const SurfaceBindingCommand &command);
    uint64_t m_curSurfaceStage[4] = {};
    uint8_t m_curSurfaceUnnormalized[4] = {};
    uint32_t m_currentTarget = 0;
    uint32_t m_currentTargetWidth = 0;
    uint32_t m_currentTargetHeight = 0;
    uint64_t m_currentColorGeneration = 0;
    uint64_t m_currentZetaGeneration = 0;
    uint64_t m_currentFramebufferGeneration = 0;
    uint32_t m_currentZetaFormat = XGPU_ZETA_NONE;
    uint32_t m_currentZetaFloat = 0;
    uint32_t m_presentTarget = 0;

    bool m_progReady = false;
    std::unique_ptr<::plume::RenderShader> m_progVS[20];
    std::unique_ptr<::plume::RenderPipelineLayout> m_progLayout;
    std::unique_ptr<::plume::RenderSampler> m_progSampler;
    std::unordered_map<uint64_t, std::unique_ptr<::plume::RenderSampler>> m_progSamplers;
    XgpuSamplerBinding m_curSamplerState[4] = {};
    uint8_t m_curSamplerStateValid[4] = {};
    std::unique_ptr<::plume::RenderTexture> m_whiteTex;
    std::unique_ptr<::plume::RenderTextureView> m_whiteView;
    std::unordered_map<uint64_t, std::unique_ptr<::plume::RenderPipeline>> m_progPsos;
    /* Live-reload replacements can race up to three asynchronous WAIT
     * submissions. Retain superseded GPU objects for the process lifetime so
     * no backend observes a destroyed shader or PSO still in flight. */
    std::vector<std::unique_ptr<::plume::RenderShader>>
        m_liveRetiredPixelShaders;
    std::vector<std::unique_ptr<::plume::RenderPipeline>>
        m_liveRetiredPipelines;
    uint64_t m_liveShaderPollMs = 0;
    std::map<std::array<uint64_t, 10>, std::unique_ptr<::plume::RenderDescriptorSet>> m_progDescCache;
    /* GPU vertex-transform path: NV2A vertex programs (SET_TRANSFORM_PROGRAM
     * microcode) translated to a Plume vertex shader via the d3d8_vsh HLSL
     * generator, compiled with DXC and cached by microcode hash. Replaces the
     * per-vertex CPU interpreter (execute_transform_program) for supported
     * programs. */
    struct PlumeVertexProgram {
        std::vector<uint8_t> bytecode;
        std::string entryPoint = "main";
        ShaderTarget target = ShaderTarget::DXIL;
        std::unique_ptr<::plume::RenderShader> shader;
        bool ok = false;
        bool compiled = false;
        uint16_t inputsRead = 0;   /* v0-v15 the microcode reads */
    };
    std::unordered_map<uint64_t, PlumeVertexProgram> m_vpCache;
    struct GuestVertexProgram {
        uint16_t inputsRead = 0;
        bool ok = false;
        std::future<ShaderCompileResult> compileFuture;
    };
    std::unordered_map<uint64_t, GuestVertexProgram> m_guestVertexPrograms;
    ShaderTarget m_recordShaderTarget = ShaderTarget::DXIL;
    uint64_t m_curVertexProgramKey = 0;
    uint16_t m_curVertexInputs = 0;   /* inputsRead of the latched program */

public:
    /* Guest recorder: queue portable bytecode compilation and use the CPU
     * fallback until it completes. Only replay creates the RHI shader object. */
    int setVertexProgram(const uint32_t *microcode, uint32_t length,
                         const uint32_t *vertexFormat);
    /* Defer an indexed programmable draw (GPU vertex transform). Returns false
     * if unsupported so the caller falls back to the CPU interpreter. */
    XgpuPlumeGpuDrawResult recordProgIndexedDraw(
        PlumeContext &ctx, const XgpuProgIndexedDraw &desc);
private:
    struct ProgDraw;   /* defined below; used by progIdxPso */
    /* Build/cache the input layout + PSO for an indexed programmable draw. */
    ::plume::RenderPipeline *progIdxPso(PlumeContext &ctx, const ProgDraw &d,
                                        bool hasDepthAttachment);
    PlumeTex *resolveTextureBinding(const RecordedTextureBinding &binding);
    const PlumeTex *resolveTextureBinding(
        const RecordedTextureBinding &binding) const;
    ::plume::RenderSampler *samplerForBinding(
        PlumeContext &ctx, const XgpuSamplerBinding &binding, bool valid);

    std::unordered_map<uint32_t, PlumePixelShader> m_psReg;
    std::unordered_map<std::string, uint32_t> m_psByText;
    uint32_t m_psNext = 1;
    uint32_t m_fixedFallbackPS = 0;
    uint32_t m_fixedFallbackPSW = 0;
    uint32_t m_activePS = 0;
    float m_psConst[8][4] = {{0}};
    float m_combinerConst[21][4] = {{0}};
    uint64_t m_psConstVersion = 1;
    uint64_t m_combinerConstVersion = 1;
    std::array<float, 16> snapshotProgramTextureScales() const;
    std::array<float, 100> snapshotProgramConstants(
        bool combinerCB, const std::array<float, 16> &textureScales) const;
    uint32_t internProgramConstants(bool combinerCB);
    std::unordered_map<uint32_t, PlumeVertexShader> m_vsReg;
    uint32_t m_activeVS = 0;
    float m_vsConst[192][4] = {{0}};
    uint64_t m_vsConstVersion = 1;
    float m_vertexData[16][4] = {{0}};

    bool m_stickyHostFrame = false;
    bool m_recordingHostFrame = false;
    bool m_recordingHostOverlay = false;
    PlumeFrameDrawCounter m_frameDrawCounter;
};

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_DRAW_H */
