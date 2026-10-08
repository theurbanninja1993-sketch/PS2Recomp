#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gpu/gl_context.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class GSCpuBackend;

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

        struct Program
        {
            GLuint id = 0;
            std::unordered_map<const char *, GLint> locations;
        };

        void initGpuObjects();
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

        // CPU copy of local memory <-> VRAM.
        void readbackVram();
        void uploadVram(uint32_t beginByte, uint32_t endByte);
        void dispatchHostToLocal(const uint8_t *data, uint32_t pixels, uint32_t streamBytes);
        void dispatchLocalToLocal(const GSTransferCommand &command);

        GLContext m_context;
        bool m_gpu = false;
        std::unique_ptr<GSCpuBackend> m_cpu; // presentation, local->host transfers
        uint8_t *m_vram = nullptr;

        std::vector<std::unique_ptr<Target>> m_targets;
        std::unordered_map<uint64_t, GLuint> m_framebuffers; // (color tex, depth tex) -> FBO

        GLuint m_vramBuffer = 0, m_swizzleBuffer = 0, m_clutBuffer = 0, m_stagingBuffer = 0;
        size_t m_stagingCapacity = 0;
        Program m_draw, m_loadColor, m_loadDepth, m_resolve, m_hostToLocal, m_localToLocal, m_clutLoad;
        GLuint m_vao = 0, m_emptyVao = 0;
        GLuint m_vbo = 0;
        size_t m_vboCapacity = 0;
        std::vector<Vertex> m_vertices;
        bool m_haveKey = false;
        bool m_inFlush = false; // applyDrawState may resolve memory mid-flush
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
