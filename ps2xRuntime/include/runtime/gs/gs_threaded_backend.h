#pragma once

#include "runtime/gs/gs_backend.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Runs another raster backend on a worker thread. Drawing, CLUT loads and
// host->local transfers are queued in order; anything that observes GS local
// memory (presentation, local->host readback, VRAM reads/writes, resets) waits
// for the queue to drain first, so results match the synchronous backend.
class GSThreadedBackend final : public GSRasterBackend
{
public:
    explicit GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner);
    ~GSThreadedBackend() override;

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
        Flush,
        TextureFlush,
        Present,
    };

    struct Command
    {
        CommandType type = CommandType::Flush;
        GSPrimitiveBatch batch{};
        GSTex0Reg tex0{};
        GSTexClutReg texclut{};
        GSTransferCommand transfer{};
        GSPresentationRequest presentRequest{};
        std::vector<uint8_t> data;
    };

    void enqueue(Command &&command);
    void drain() const;
    void workerLoop();
    void execute(Command &command);

    std::unique_ptr<GSRasterBackend> m_inner;
    mutable std::mutex m_queueMutex;
    mutable std::condition_variable m_workAvailable;
    mutable std::condition_variable m_idle;
    std::vector<Command> m_queue;
    bool m_busy = false;
    std::mutex m_presentMutex;
    PresentationFrame m_lastPresented{};
    bool m_stop = false;
    std::thread m_worker;
};
