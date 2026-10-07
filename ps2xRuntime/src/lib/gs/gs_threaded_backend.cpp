#include "runtime/gs/gs_threaded_backend.h"

#include <utility>

namespace
{
    // Bound on queued-but-unrendered work. Without it a producer that outruns
    // the rasterizer (e.g. movie frame uploads) grows the queue without limit.
    constexpr size_t kMaxQueuedBytes = 64u * 1024u * 1024u;
}

GSThreadedBackend::GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner)
    : m_inner(std::move(inner))
{
    m_worker = std::thread([this]()
                           { workerLoop(); });
}

GSThreadedBackend::~GSThreadedBackend()
{
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stop = true;
    }
    m_workAvailable.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void GSThreadedBackend::enqueue(Command &&command)
{
    bool wake = false;
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        if (m_queuedBytes >= kMaxQueuedBytes)
        {
            m_workAvailable.notify_one();
            m_batchTaken.wait(lock, [this]()
                              { return m_queuedBytes < kMaxQueuedBytes; });
        }
        wake = m_queue.empty() && !m_busy;
        m_queuedBytes += sizeof(Command) + command.data.size();
        m_queue.push_back(std::move(command));
    }
    if (wake)
        m_workAvailable.notify_one();
}

void GSThreadedBackend::drain() const
{
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_idle.wait(lock, [this]()
                { return m_queue.empty() && !m_busy; });
}

void GSThreadedBackend::workerLoop()
{
    std::vector<Command> batch;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_busy = false;
            if (m_queue.empty())
                m_idle.notify_all();
            m_workAvailable.wait(lock, [this]()
                                 { return m_stop || !m_queue.empty(); });
            if (m_stop && m_queue.empty())
                return;
            batch.swap(m_queue);
            m_queuedBytes = 0;
            m_busy = true;
        }
        m_batchTaken.notify_all();
        for (Command &command : batch)
            execute(command);
        batch.clear();
    }
}

void GSThreadedBackend::execute(Command &command)
{
    switch (command.type)
    {
    case CommandType::Submit:
        m_inner->Submit(command.batch);
        break;
    case CommandType::LoadClut:
        m_inner->LoadClut(command.tex0, command.texclut);
        break;
    case CommandType::BeginTransfer:
        m_inner->BeginTransfer(command.transfer);
        break;
    case CommandType::UploadImage:
        m_inner->UploadImage(command.data.data(), static_cast<uint32_t>(command.data.size()));
        break;
    case CommandType::Flush:
        m_inner->Flush();
        break;
    case CommandType::TextureFlush:
        m_inner->TextureFlush();
        break;
    case CommandType::Present:
    {
        m_inner->Sync(GSSyncReason::Presentation);
        PresentationFrame frame = m_inner->Present(command.presentRequest);
        std::lock_guard<std::mutex> lock(m_presentMutex);
        m_lastPresented = std::move(frame);
        break;
    }
    }
}

void GSThreadedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    drain();
    m_inner->Initialize(vram, vramSize);
}

void GSThreadedBackend::Reset()
{
    drain();
    m_inner->Reset();
}

void GSThreadedBackend::Submit(const GSPrimitiveBatch &batch)
{
    Command command;
    command.type = CommandType::Submit;
    command.batch = batch;
    enqueue(std::move(command));
}

void GSThreadedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    Command command;
    command.type = CommandType::LoadClut;
    command.tex0 = tex0;
    command.texclut = texclut;
    enqueue(std::move(command));
}

void GSThreadedBackend::BeginTransfer(const GSTransferCommand &command)
{
    Command queued;
    queued.type = CommandType::BeginTransfer;
    queued.transfer = command;
    enqueue(std::move(queued));
}

void GSThreadedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Command command;
    command.type = CommandType::UploadImage;
    command.data.assign(data, data + sizeBytes);
    enqueue(std::move(command));
}

void GSThreadedBackend::Flush()
{
    Command command;
    command.type = CommandType::Flush;
    enqueue(std::move(command));
}

void GSThreadedBackend::TextureFlush()
{
    Command command;
    command.type = CommandType::TextureFlush;
    enqueue(std::move(command));
}

void GSThreadedBackend::Sync(GSSyncReason reason)
{
    // FINISH only raises the CSR flag (done by the frontend), and presentation
    // is captured in order on the worker (see Present); waiting for the
    // rasterizer in either case would serialize the EE with the GS every frame.
    if (reason == GSSyncReason::Finish || reason == GSSyncReason::Presentation)
        return;
    drain();
    m_inner->Sync(reason);
}

PresentationFrame GSThreadedBackend::Present(const GSPresentationRequest &request)
{
    // Capture the display buffer at this point of the command stream on the
    // worker, and hand back the most recently completed capture (at most one
    // vblank behind) instead of stalling the EE until the GS catches up.
    Command command;
    command.type = CommandType::Present;
    command.presentRequest = request;
    enqueue(std::move(command));
    std::lock_guard<std::mutex> lock(m_presentMutex);
    return m_lastPresented;
}

bool GSThreadedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    drain();
    return m_inner->ClearFramebuffer(context, rgba);
}

uint32_t GSThreadedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    drain();
    return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSThreadedBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    drain();
    return m_inner->ReadVram(psm, base, bw, x, y);
}

void GSThreadedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    drain();
    m_inner->WriteVram(psm, base, bw, x, y, value);
}

void GSThreadedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    drain();
    m_inner->SnapshotVram(out);
}

GSTransferSnapshot GSThreadedBackend::GetTransferSnapshot() const
{
    // Debug bookkeeping only; reading the inner backend's (mutex-guarded)
    // transfer state without draining keeps uploads asynchronous.
    return m_inner->GetTransferSnapshot();
}
