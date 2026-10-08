#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gpu/gl_context.h"

#include <memory>
#include <vector>

class GSCpuBackend;

namespace gsgpu
{
    // GS rendering state that lives on the GPU backend's render thread.
    // Phase 1: owns the GL context and delegates rasterization to an inner
    // CPU backend; later phases move primitives onto the GPU.
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

        [[nodiscard]] bool gpuAvailable() const { return m_context.valid(); }

    private:
        GLContext m_context;
        std::unique_ptr<GSCpuBackend> m_cpu; // reference path / fallback
    };
}
