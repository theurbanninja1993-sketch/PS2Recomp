#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gpu/gl_context.h"
#include "runtime/gs/gs_texture_replacements.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class GSCpuBackend;

// GL state calls the state cache skipped (defined in gs_gpu_renderer.cpp).
extern std::atomic<uint64_t> g_gsGpuStateSkips;

namespace gsgpu
{
    // GS rendering state that lives on the GPU backend's render thread.
    //
    // Local memory: a 4 MiB GPU buffer ("VRAM") in the GS's swizzled layout
    // is the reference. Transfers, CLUT loads and texture sampling operate on
    // it directly in shaders. Frame and Z buffers drawn by the GPU live in GL
    // textures ("targets"); each tracks rows it drew that VRAM lacks (resolved
    // into VRAM on demand by a compute pass) and rows VRAM changed since it
    // was filled (reloaded before it is drawn). The CPU copy of local memory
    // (shared with an inner GSCpuBackend, which presents frames) is refreshed
    // from VRAM only when the CPU needs it.
    class Renderer
    {
    public:
        Renderer();
        ~Renderer();

        void initialize(uint8_t *vram, uint32_t vramSize);
        void reset();

        void submit(const GSPrimitiveBatch &batch);
        void loadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
        void beginTransfer(const GSTransferCommand &command);
        void uploadImage(const uint8_t *data, uint32_t sizeBytes);
        void textureFlush();

        // Brings CPU local memory up to date with the GPU.
        void syncLocalMemory();
        PresentationFrame present(const GSPresentationRequest &request);
        bool clearFramebuffer(const GSContext &context, uint32_t rgba);
        uint32_t consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes);

        uint32_t readVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
        void writeVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
        void snapshotVram(std::vector<uint8_t> &out);
        GSTransferSnapshot transferSnapshot();

        [[nodiscard]] bool gpuAvailable() const { return m_gpu; }

        // Internal resolution: render targets are `scale` times the GS size
        // in each direction (1 = native). Changing it rebuilds the targets
        // from local memory.
        void setScale(uint32_t scale);

    private:
        struct Target
        {
            bool depth = false;
            uint32_t block = 0; // base address in 256-byte blocks
            uint32_t bw = 0;    // buffer width in 64-pixel units
            uint8_t psm = 0;
            uint32_t width = 0, height = 0;
            // Rows [0, usedRows) have been drawn or are about to be; the texture
            // only mirrors memory there, and [beginByte, endByte) covers them.
            uint32_t usedRows = 0;
            uint32_t beginByte = 0, endByte = 0;
            GLuint texture = 0;
            GLuint loadFramebuffer = 0; // the texture alone, for load passes
            // Rows the GPU drew that VRAM lacks, and rows whose texture
            // contents are stale (VRAM changed). A target never has both.
            uint32_t dirtyY0 = ~0u, dirtyY1 = 0;
            uint32_t staleY0 = ~0u, staleY1 = 0;
            uint64_t drawSeq = 0; // last draw into it, to order overlapping resolves

            [[nodiscard]] bool gpuDirty() const { return dirtyY0 <= dirtyY1; }
            [[nodiscard]] bool stale() const { return staleY0 <= staleY1; }
        };

        struct Vertex
        {
            float x, y;
            uint32_t z;
            uint32_t rgba;
            float s, t, q;
            float fog;
        };

        // Everything that selects GL state / uniforms for a draw.
        struct DrawKey
        {
            uint32_t frameBlock, frameBw, framePsm, fbmsk;
            uint32_t zBlock, zPsm, zmask, zte;
            int32_t scissor[4];
            uint64_t test, alpha, clamp, tex1, fba;
            uint32_t tme, fge, abe, pabe;
            uint32_t tbp0, tbw, tpsm, tw, th, tcc, tfx, cbp, cpsm, csm, csa;
            uint64_t clutVersion;
            uint32_t ta0, ta1, aem;
            uint32_t fogColor;
            uint32_t linear;
            bool operator==(const DrawKey &other) const;
        };

        // Uniform values, cached per program and indexed by location so the
        // lookup is a vector index rather than a map probe on the hot path.
        // Uniforms are per-program (glProgramUniform*), so a cached value is
        // only invalidated by another glProgramUniform on the same program -
        // nothing outside the renderer can touch these.
        enum class UniformKind : uint8_t { Int, Uint, Float };
        struct UniformSlot
        {
            uint8_t valid = 0;
            uint8_t count = 0;
            UniformKind kind = UniformKind::Int;
            uint32_t bits[4] = {};
        };
        struct Program
        {
            GLuint id = 0;
            std::unordered_map<const char *, GLint> locations;
            std::vector<UniformSlot> uniforms;
        };
        // Sets a uniform only if it differs. Bit comparison, so floats compare
        // exactly (the same value is stored bit-identically).
        void setUniformBits(Program &program, GLint location, UniformKind kind, uint32_t count, const uint32_t *bits);
        void setUniform1i(Program &p, GLint l, int32_t v) { const uint32_t b[1] = {static_cast<uint32_t>(v)}; setUniformBits(p, l, UniformKind::Int, 1, b); }
        void setUniform2i(Program &p, GLint l, int32_t a, int32_t c) { const uint32_t b[2] = {static_cast<uint32_t>(a), static_cast<uint32_t>(c)}; setUniformBits(p, l, UniformKind::Int, 2, b); }
        void setUniform4i(Program &p, GLint l, int32_t a, int32_t c, int32_t d, int32_t e) { const uint32_t b[4] = {static_cast<uint32_t>(a), static_cast<uint32_t>(c), static_cast<uint32_t>(d), static_cast<uint32_t>(e)}; setUniformBits(p, l, UniformKind::Int, 4, b); }
        void setUniform1ui(Program &p, GLint l, uint32_t a) { const uint32_t b[1] = {a}; setUniformBits(p, l, UniformKind::Uint, 1, b); }
        void setUniform2ui(Program &p, GLint l, uint32_t a, uint32_t c) { const uint32_t b[2] = {a, c}; setUniformBits(p, l, UniformKind::Uint, 2, b); }
        void setUniform3ui(Program &p, GLint l, uint32_t a, uint32_t c, uint32_t d) { const uint32_t b[3] = {a, c, d}; setUniformBits(p, l, UniformKind::Uint, 3, b); }
        void setUniform4ui(Program &p, GLint l, uint32_t a, uint32_t c, uint32_t d, uint32_t e) { const uint32_t b[4] = {a, c, d, e}; setUniformBits(p, l, UniformKind::Uint, 4, b); }
        void setUniform1f(Program &p, GLint l, float v) { uint32_t b[1]; std::memcpy(b, &v, 4); setUniformBits(p, l, UniformKind::Float, 1, b); }
        void setUniform2f(Program &p, GLint l, float a, float c) { uint32_t b[2]; std::memcpy(b, &a, 4); std::memcpy(b + 1, &c, 4); setUniformBits(p, l, UniformKind::Float, 2, b); }
        void setUniform3f(Program &p, GLint l, float a, float c, float d) { uint32_t b[3]; std::memcpy(b, &a, 4); std::memcpy(b + 1, &c, 4); std::memcpy(b + 2, &d, 4); setUniformBits(p, l, UniformKind::Float, 3, b); }
        void setUniform4f(Program &p, GLint l, float a, float c, float d, float e) { uint32_t b[4]; std::memcpy(b, &a, 4); std::memcpy(b + 1, &c, 4); std::memcpy(b + 2, &d, 4); std::memcpy(b + 3, &e, 4); setUniformBits(p, l, UniformKind::Float, 4, b); }

        void initGpuObjects();
        // Clears every program's cached uniform values. initGpuObjects() only
        // reassigns .id, so a recreated context would otherwise keep values
        // cached against the old program objects and skip setting them.
        void clearUniformCaches()
        {
            for (Program *p : {&m_draw, &m_loadColor, &m_loadDepth, &m_resolve, &m_hostToLocal, &m_localToLocal,
                               &m_clutLoad, &m_compose})
                p->uniforms.clear();
        }
        void flushBatch();
        void applyDrawState(const DrawKey &key, const GSDrawState &state);
        GLint uniform(Program &program, const char *name);

        Target &colorTarget(const GSFrameReg &frame);
        Target &depthTarget(const GSZbufReg &zbuf, uint32_t bw);
        Target &findOrCreateTarget(bool depth, uint32_t block, uint32_t bw, uint8_t psm);
        GLuint framebufferFor(const Target &color, const Target *depth);
        void loadRows(Target &target, uint32_t y0, uint32_t rows); // VRAM -> texture
        void resolveTarget(Target &target);                        // texture -> VRAM (dirty rows)
        // Before VRAM [begin, end) is read: resolve targets drawn there.
        void resolveRange(uint32_t beginByte, uint32_t endByte);
        // After VRAM [begin, end) changed: targets there are stale.
        void vramWroteRange(uint32_t beginByte, uint32_t endByte);
        // Before the GPU draws rows [drawY0, drawY1] of `target`: refresh
        // it, and retire other targets aliasing that memory.
        void prepareTargetForDraw(Target &target, int32_t drawY0, int32_t drawY1);
        // Mirror more rows of `target` (fetched from VRAM).
        void growTarget(Target &target, uint32_t needRows);
        // Memory bytes holding rows [y0, y1] of `target` (whole page rows).
        void rowsToBytes(const Target &target, uint32_t y0, uint32_t y1, uint32_t &begin, uint32_t &end) const;
        // Rows of `target` (within its used rows) stored in [begin, end).
        bool bytesToRows(const Target &target, uint32_t begin, uint32_t end, uint32_t &y0, uint32_t &y1) const;
        void markStaleRows(Target &target, uint32_t beginByte, uint32_t endByte);
        void loadStaleRows(Target &target);
        // Colour target a draw can sample directly (instead of local memory)
        // for texture TBP0/TBW/PSM covering `rows`; brought up to date.
        Target *textureSourceTarget(uint32_t tbp0, uint32_t tbw, uint32_t tpsm, uint32_t rows);
        // Scaled copy of `target` for draws sampling the target they render to.
        GLuint feedbackCopy(const Target &target);
        // High-resolution version of the CPU-composed presentation frame.
        void upscalePresentation(const GSPresentationRequest &request, PresentationFrame &frame);
        void applyPendingScale();
        void destroyTargets();

        // Texture replacement (PCSX2-format packs, gs_texture_replacements.h):
        // texture memory is hashed from the CPU copy of local memory, kept in
        // step with uploads while a pack is loaded; replacement images load
        // on a worker thread and are sampled in place of the GS texture.
        struct Replacement
        {
            GLuint texture = 0;
            bool requested = false;
            uint64_t stamp = 0; // the file's stamp the texture was loaded from
        };
        struct HashedTexture
        {
            uint64_t generation = 0; // 1 + sum of its pages' write generations when hashed
            uint64_t hash = 0;
        };
        void updateReplacements(); // between frames: a new set of packs
        bool replacementsActive() const { return m_replIndex && !m_replIndex->files.empty(); }
        GLuint replacementTexture(const GSDrawState &state);
        // PS2X_TEXREPL_DUMP=<dir>: write every texture the matcher sees as a
        // PNG named by its replacement key (<tex0Hash>-<clutHash>-<bits>.png),
        // the same naming PCSX2 uses. For authoring packs (e.g. button-prompt
        // sets); the RGBA expansion is best effort for identification.
        void dumpReplacementTexture(const GSDrawState &state, const gsrepl::Key &key);
        uint64_t hashTextureMemory(const GSDrawState &state);
        uint64_t hashClut(const GSDrawState &state);
        // CPU-copy bookkeeping for [begin, end): a write happened; `cpuStale`:
        // only VRAM holds it (GPU writes) until the next readback.
        void markPagesWritten(uint32_t beginByte, uint32_t endByte, bool cpuStale);
        void replacementWorker();
        void uploadLoadedReplacements();
        void stopReplacementWorker();

        // CPU copy of local memory <-> VRAM.
        void readbackVram();
        void uploadVram(uint32_t beginByte, uint32_t endByte);
        void dispatchHostToLocal(const uint8_t *data, uint32_t pixels, uint32_t streamBytes);
        void dispatchLocalToLocal(const GSTransferCommand &command);

        GLContext m_context;
        bool m_gpu = false;
        std::unique_ptr<GSCpuBackend> m_cpu; // presentation, local->host transfers
        uint8_t *m_vram = nullptr;

        std::shared_ptr<const gsrepl::Index> m_replIndex;
        uint64_t m_replVersion = 0;
        std::unordered_map<gsrepl::Key, Replacement, gsrepl::KeyHash> m_replacements;
        std::unordered_map<uint64_t, HashedTexture> m_textureHashes;
        std::unordered_set<gsrepl::Key, gsrepl::KeyHash> m_dumpedTextures; // PS2X_TEXREPL_DUMP
        std::vector<uint32_t> m_pageGeneration = std::vector<uint32_t>(512, 0u); // 8 KiB pages of local memory
        std::vector<uint8_t> m_pageCpuStale = std::vector<uint8_t>(512, 0u);
        uint64_t m_clutHashKey = ~0ull, m_clutHash = 0;
        std::thread m_replThread;
        std::mutex m_replMutex;
        std::condition_variable m_replCv;
        bool m_replStop = false;
        std::deque<std::pair<gsrepl::Key, std::string>> m_replRequests;
        std::deque<std::pair<gsrepl::Key, std::unique_ptr<gsrepl::Image>>> m_replResults;
        std::atomic<uint32_t> m_replResultCount{0};
        // Loaded replacements waiting for their GPU upload (render thread only),
        // and the bytes uploaded this frame (a burst is spread over frames).
        std::deque<std::pair<gsrepl::Key, std::unique_ptr<gsrepl::Image>>> m_replPendingUploads;
        size_t m_replUploadedThisFrame = 0;
        uint64_t m_statReplDraws = 0, m_statReplHits = 0, m_statReplMisses = 0, m_statReplLoaded = 0, m_statReplHashes = 0;
        // PS2X_TEXREPL_STATS: prints the accumulated replacement-related
        // render-thread counters (called at the interval and at teardown).
        void emitTexreplStats();

        uint32_t m_scale = 1;
        std::atomic<uint32_t> m_requestedScale{1};
        GLuint m_feedbackTexture = 0;
        uint32_t m_feedbackWidth = 0, m_feedbackHeight = 0;
        std::vector<std::unique_ptr<Target>> m_targets;
        std::unordered_map<uint64_t, GLuint> m_framebuffers; // (color tex, depth tex) -> FBO

        GLuint m_vramBuffer = 0, m_swizzleBuffer = 0, m_clutBuffer = 0, m_stagingBuffer = 0;
        size_t m_stagingCapacity = 0;
        Program m_draw, m_loadColor, m_loadDepth, m_resolve, m_hostToLocal, m_localToLocal, m_clutLoad, m_compose;
        GLuint m_presentTexture = 0, m_presentFramebuffer = 0; // upscaled display output
        uint32_t m_presentWidth = 0, m_presentHeight = 0;
        // Downscale target for the presentation readback (see upscalePresentation).
        GLuint m_readbackTexture = 0, m_readbackFramebuffer = 0;
        uint32_t m_readbackWidth = 0, m_readbackHeight = 0;
        // Double-buffered composed display handed to the host as a texture when
        // the renderer shares the window's GL context (no readback, no upload).
        GLuint m_presentShared[2] = {0u, 0u};
        GLuint m_presentSharedFramebuffer[2] = {0u, 0u};
        uint32_t m_presentSharedWidth = 0, m_presentSharedHeight = 0;
        GLint m_presentSharedFilter = 0; // GL_LINEAR / GL_NEAREST, follows the setting
        int m_presentSharedIndex = 0;
        GLuint m_vao = 0, m_emptyVao = 0;
        GLuint m_vbo = 0;
        size_t m_vboCapacity = 0;
        std::vector<Vertex> m_vertices;
        bool m_haveKey = false;
        bool m_inFlush = false; // applyDrawState may resolve memory mid-flush
        // resolveTarget issues the two glMemoryBarrier calls a resolve needs,
        // but only once per *group*: consecutive resolves in one call site are
        // ordered by a single pair. Two barriers per resolve cost ~6 driver
        // points at 250-350 draws/frame (owner-measured, dropping both gains
        // 2.1 fps), and the spec only needs one pair around the writes.
        bool m_resolveGroupOpen = false;
        // A resolve's read of a target texture only needs ordering if something
        // drew into that framebuffer since the last barrier. Every resolve site
        // resolves ~1 target, so per-site grouping coalesces nothing (measured);
        // this is what actually removes barriers - a draw followed by several
        // resolves costs one barrier, not one each.
        bool m_framebufferWriteSinceBarrier = true;

        // GL state cache for the four calls every pass re-issues whether or not
        // they changed. Consecutive passes overwhelmingly use the same program,
        // framebuffer, viewport and VAO (all 8,349 resolves share one program;
        // consecutive draws share a target), so this skips the redundant ones.
        // Driver-independent by construction: fewer calls is fewer calls, which
        // is why the skip count is the number to measure locally.
        //
        // Sentinels rather than a valid flag: no GL name is 0xFFFFFFFF and no
        // viewport component is INT32_MIN, so the first call always applies.
        struct GlStateCache
        {
            uint32_t program = ~0u, framebuffer = ~0u, vao = ~0u;
            int32_t viewport[4] = {INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN};
            int32_t scissor[4] = {INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN};
            int32_t scissorTest = -1, depthTest = -1, blend = -1;
            int32_t depthFunc = -1, depthMask = -1;
            int32_t colorMask[4] = {-1, -1, -1, -1};
        };
        GlStateCache m_glState;
        uint64_t m_glStateSkips = 0; // GL state calls the cache avoided
        void invalidateGlState() { m_glState = GlStateCache{}; }
        void useProgram(GLuint program)
        {
            if (m_glState.program == program) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.program = program;
            m_context.gl.glUseProgram(program);
        }
        void bindFramebuffer(GLuint framebuffer)
        {
            if (m_glState.framebuffer == framebuffer) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.framebuffer = framebuffer;
            m_context.gl.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        }
        void bindVao(GLuint vao)
        {
            if (m_glState.vao == vao) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.vao = vao;
            m_context.gl.glBindVertexArray(vao);
        }
        void setScissorTest(bool on)
        {
            const int32_t v = on ? 1 : 0;
            if (m_glState.scissorTest == v) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.scissorTest = v;
            if (on) m_context.gl.glEnable(GL_SCISSOR_TEST); else m_context.gl.glDisable(GL_SCISSOR_TEST);
        }
        void setScissor(int32_t x, int32_t y, int32_t w, int32_t h)
        {
            if (m_glState.scissor[0] == x && m_glState.scissor[1] == y && m_glState.scissor[2] == w &&
                m_glState.scissor[3] == h)
            { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.scissor[0] = x; m_glState.scissor[1] = y; m_glState.scissor[2] = w; m_glState.scissor[3] = h;
            m_context.gl.glScissor(x, y, w, h);
        }
        void setDepthTest(bool on)
        {
            const int32_t v = on ? 1 : 0;
            if (m_glState.depthTest == v) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.depthTest = v;
            if (on) m_context.gl.glEnable(GL_DEPTH_TEST); else m_context.gl.glDisable(GL_DEPTH_TEST);
        }
        void setBlend(bool on)
        {
            const int32_t v = on ? 1 : 0;
            if (m_glState.blend == v) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.blend = v;
            if (on) m_context.gl.glEnable(GL_BLEND); else m_context.gl.glDisable(GL_BLEND);
        }
        void setDepthFunc(int32_t func)
        {
            if (m_glState.depthFunc == func) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.depthFunc = func;
            m_context.gl.glDepthFunc(static_cast<GLenum>(func));
        }
        void setDepthMask(int32_t mask)
        {
            if (m_glState.depthMask == mask) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.depthMask = mask;
            m_context.gl.glDepthMask(static_cast<GLboolean>(mask));
        }
        void setColorMask(int32_t r, int32_t g, int32_t b, int32_t a)
        {
            if (m_glState.colorMask[0] == r && m_glState.colorMask[1] == g && m_glState.colorMask[2] == b &&
                m_glState.colorMask[3] == a)
            { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.colorMask[0] = r; m_glState.colorMask[1] = g; m_glState.colorMask[2] = b; m_glState.colorMask[3] = a;
            m_context.gl.glColorMask(static_cast<GLboolean>(r), static_cast<GLboolean>(g), static_cast<GLboolean>(b),
                                     static_cast<GLboolean>(a));
        }
        void setViewport(int32_t x, int32_t y, int32_t w, int32_t h)
        {
            if (m_glState.viewport[0] == x && m_glState.viewport[1] == y && m_glState.viewport[2] == w &&
                m_glState.viewport[3] == h)
            {
                ++m_glStateSkips;
                ++g_gsGpuStateSkips;
                return;
            }
            m_glState.viewport[0] = x; m_glState.viewport[1] = y; m_glState.viewport[2] = w; m_glState.viewport[3] = h;
            m_context.gl.glViewport(x, y, w, h);
        }
        // A resolve's VRAM write only needs ordering before the next VRAM read.
        // Every reader funnels through loadRows() and readbackVram(), so the
        // barrier is issued there - one per read instead of one per resolve.
        bool m_vramWriteSinceBarrier = false;
        void vramBarrierBeforeRead();
        void resolveBarrierBegin();
        void resolveGroupEnd();
        DrawKey m_key{};
        GSDrawState m_keyState{};
        Target *m_drawColor = nullptr;
        Target *m_drawDepth = nullptr;
        int32_t m_drawY0 = 0, m_drawY1 = 0;
        int32_t m_batchY0 = 0, m_batchY1 = 0; // vertex row bounds of the pending batch
        uint64_t m_drawSeq = 0;

        // CLUT: CBP0/CBP1 registers for the CLD compare modes, and a version
        // bumped per load so batches don't straddle one.
        uint32_t m_clutCbp[2] = {0, 0};
        uint64_t m_clutVersion = 0;

        // Current host->local transfer.
        GSTransferCommand m_transfer{};
        GSTransferSnapshot m_transferState{};
        bool m_cpuTransfer = false; // local->host: served by m_cpu

        uint64_t m_statDraws = 0, m_statResolves = 0, m_statLoads = 0, m_statReadbacks = 0, m_statClutLoads = 0,
                 m_statTransfers = 0;
        const char *m_syncReason = "other";
        std::unordered_map<std::string, uint64_t> m_statResolveReasons;
        double m_timeReadback = 0, m_timeDraw = 0, m_timePresent = 0;
    };
}
