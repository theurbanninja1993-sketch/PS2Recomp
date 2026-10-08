#include "runtime/gs/gpu/gs_gpu_renderer.h"

#include "runtime/gs/gs_cpu_backend.h"

#include <iostream>
#include <string>

namespace gsgpu
{
    Renderer::Renderer()
        : m_cpu(std::make_unique<GSCpuBackend>())
    {
        std::string error;
        if (m_context.create(error))
        {
            const auto *renderer = reinterpret_cast<const char *>(m_context.gl.glGetString(GL_RENDERER));
            const auto *version = reinterpret_cast<const char *>(m_context.gl.glGetString(GL_VERSION));
            std::cerr << "[gs:gpu] OpenGL " << (version ? version : "?") << " on " << (renderer ? renderer : "?")
                      << std::endl;
        }
        else
        {
            std::cerr << "[gs:gpu] no GPU context (" << error << "); using the CPU rasterizer" << std::endl;
        }
    }

    Renderer::~Renderer() = default;

    void Renderer::initialize(uint8_t *vram, uint32_t vramSize)
    {
        m_cpu->Initialize(vram, vramSize);
    }

    void Renderer::reset()
    {
        m_cpu->Reset();
    }

    void Renderer::submit(const GSPrimitiveBatch &batch)
    {
        m_cpu->Submit(batch);
    }

    void Renderer::loadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
    {
        m_cpu->LoadClut(tex0, texclut);
    }

    void Renderer::beginTransfer(const GSTransferCommand &command)
    {
        m_cpu->BeginTransfer(command);
    }

    void Renderer::uploadImage(const uint8_t *data, uint32_t sizeBytes)
    {
        m_cpu->UploadImage(data, sizeBytes);
    }

    void Renderer::textureFlush()
    {
        m_cpu->TextureFlush();
    }

    void Renderer::syncLocalMemory()
    {
    }

    PresentationFrame Renderer::present(const GSPresentationRequest &request)
    {
        m_cpu->Sync(GSSyncReason::Presentation);
        return m_cpu->Present(request);
    }

    bool Renderer::clearFramebuffer(const GSContext &context, uint32_t rgba)
    {
        return m_cpu->ClearFramebuffer(context, rgba);
    }

    uint32_t Renderer::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
    {
        return m_cpu->ConsumeLocalToHostBytes(dst, maxBytes);
    }

    uint32_t Renderer::readVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
    {
        return m_cpu->ReadVram(psm, base, bw, x, y);
    }

    void Renderer::writeVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
    {
        m_cpu->WriteVram(psm, base, bw, x, y, value);
    }

    void Renderer::snapshotVram(std::vector<uint8_t> &out)
    {
        m_cpu->SnapshotVram(out);
    }

    GSTransferSnapshot Renderer::transferSnapshot()
    {
        return m_cpu->GetTransferSnapshot();
    }
}
