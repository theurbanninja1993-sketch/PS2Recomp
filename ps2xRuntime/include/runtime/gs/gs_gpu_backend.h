#pragma once

#include "runtime/gs/gs_backend.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gsgpu
{
    class Renderer;
}

// GS raster backend that renders with the GPU (see docs/gpu-renderer.md).
// Every call is executed in order on a dedicated render thread that owns the
// GL context; calls returning data wait for the render thread.
class GSGpuBackend final : public GSRasterBackend
{
public:
    GSGpuBackend();
    ~GSGpuBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    enum class CommandType : uint8_t
    {
        Submit,
        LoadClut,
        BeginTransfer,
        UploadImage,
        TextureFlush,
        Call,
    };

    struct Command
    {
        CommandType type = CommandType::Call;
        GSPrimitiveBatch batch{};
        GSTex0Reg tex0{};
        GSTexClutReg texclut{};
        GSTransferCommand transfer{};
        std::vector<uint8_t> data;
        std::function<void()> call;
    };

    void enqueue(Command &&command);
    // Runs `fn` on the render thread after everything queued before it.
    void runSync(const std::function<void()> &fn) const;
    void renderLoop();
    void execute(Command &command);

    std::unique_ptr<gsgpu::Renderer> m_renderer; // render thread only

    mutable std::mutex m_mutex;
    mutable std::condition_variable m_workAvailable;
    mutable std::condition_variable m_space;
    mutable std::vector<Command> m_queue;
    bool m_stop = false;
    std::thread m_thread;
};
