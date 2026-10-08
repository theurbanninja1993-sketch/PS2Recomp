#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gpu/gl_context.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

class GSCpuBackend;

namespace gsgpu
{
    // GS rendering state that lives on the GPU backend's render thread.
    //
    // Local memory: the CPU copy (shared with an inner GSCpuBackend, which
    // also performs transfers, CLUT loads and presentation) is the reference.
    // Frame and Z buffers drawn by the GPU live in GL textures ("targets");
    // each target tracks whether the GPU or the CPU copy is newer and is
    // synchronized on demand: before CPU consumers read its memory
    // (presentation, texture decode, transfers) and before GPU draws use
    // memory the CPU changed.
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

        // Brings CPU local memory up to date with GPU-rendered data.
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
        enum class Owner : uint8_t
        {
            Synced,
            GpuNewer, // GL texture has data the CPU copy lacks
            CpuNewer, // CPU copy changed since the texture was filled
        };

        struct Target
        {
            bool depth = false;
            uint32_t block = 0; // base address in 256-byte blocks
            uint32_t bw = 0;    // buffer width in 64-pixel units
            uint8_t psm = 0;
            uint32_t width = 0, height = 0;
            uint32_t beginByte = 0, endByte = 0;
            GLuint texture = 0;
            Owner owner = Owner::CpuNewer;
            uint32_t dirtyY0 = ~0u, dirtyY1 = 0; // rows the GPU wrote since last download
        };

        struct CachedTexture
        {
            GLuint texture = 0;
            uint32_t width = 0, height = 0;
            uint32_t beginByte = 0, endByte = 0;
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

        void initGpuObjects();
        void flushBatch();
        void applyDrawState(const DrawKey &key, const GSDrawState &state);

        Target &colorTarget(const GSFrameReg &frame);
        Target &depthTarget(const GSZbufReg &zbuf, uint32_t bw);
        Target &findOrCreateTarget(bool depth, uint32_t block, uint32_t bw, uint8_t psm);
        GLuint framebufferFor(const Target &color, const Target *depth);
        void uploadTarget(Target &target);   // CPU -> GL
        void downloadTarget(Target &target); // GL -> CPU (dirty rows)
        // Before the CPU reads [begin, end): download GPU-newer targets there.
        void syncRangeToCpu(uint32_t beginByte, uint32_t endByte);
        // After the CPU wrote [begin, end): targets and textures there are stale.
        void cpuWroteRange(uint32_t beginByte, uint32_t endByte);
        // Before the GPU draws into `target`: refresh it, and retire other
        // targets aliasing its memory.
        void prepareTargetForDraw(Target &target);
        GLuint textureFor(const GSDrawState &state, const DrawKey &key);

        GLContext m_context;
        bool m_gpu = false;
        std::unique_ptr<GSCpuBackend> m_cpu; // transfers, CLUT, presentation, reference
        uint8_t *m_vram = nullptr;

        std::vector<std::unique_ptr<Target>> m_targets;
        std::unordered_map<uint64_t, GLuint> m_framebuffers; // (color tex, depth tex) -> FBO
        std::unordered_map<uint64_t, CachedTexture> m_textures;
        std::unordered_map<uint64_t, uint64_t> m_textureKeys; // hash -> key check

        GLuint m_program = 0;
        GLuint m_vao = 0;
        GLuint m_vbo = 0;
        size_t m_vboCapacity = 0;
        std::vector<Vertex> m_vertices;
        bool m_haveKey = false;
        bool m_inFlush = false; // applyDrawState may sync memory mid-flush
        DrawKey m_key{};
        GSDrawState m_keyState{};
        Target *m_drawColor = nullptr;
        Target *m_drawDepth = nullptr;
        int32_t m_drawY0 = 0, m_drawY1 = 0;

        // Current host->local transfer destination, for coherency.
        uint32_t m_transferBegin = 0, m_transferEnd = 0;
        bool m_transferActive = false;

        uint64_t m_statDraws = 0, m_statDownloads = 0, m_statUploads = 0, m_statTextureDecodes = 0;
    };
}
