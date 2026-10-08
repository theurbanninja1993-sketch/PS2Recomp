#include "runtime/gs/gs_gpu_backend.h"

#include "runtime/gs/gpu/gs_gpu_renderer.h"

#include <future>
#include <iostream>
#include <utility>

namespace
{
    // Bound on queued commands so the producer can't run unboundedly ahead.
    constexpr size_t kMaxQueuedCommands = 65536u;
}

GSGpuBackend::GSGpuBackend()
{
    m_thread = std::thread([this]()
                           { renderLoop(); });
}

GSGpuBackend::~GSGpuBackend()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_workAvailable.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void GSGpuBackend::enqueue(Command &&command)
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_space.wait(lock, [this]()
                     { return m_queue.size() < kMaxQueuedCommands; });
        m_queue.push_back(std::move(command));
    }
    m_workAvailable.notify_one();
}

void GSGpuBackend::runSync(const std::function<void()> &fn) const
{
    std::promise<void> done;
    Command command;
    command.type = CommandType::Call;
    command.call = [&]()
    {
        fn();
        done.set_value();
    };
    const_cast<GSGpuBackend *>(this)->enqueue(std::move(command));
    done.get_future().wait();
}

void GSGpuBackend::renderLoop()
{
    m_renderer = std::make_unique<gsgpu::Renderer>();
    std::vector<Command> batch;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workAvailable.wait(lock, [this]()
                                 { return m_stop || !m_queue.empty(); });
            if (m_queue.empty() && m_stop)
                break;
            batch.swap(m_queue);
        }
        m_space.notify_all();
        for (Command &command : batch)
            execute(command);
        batch.clear();
    }
    m_renderer.reset(); // GL objects die with the context, on this thread
}

void GSGpuBackend::execute(Command &command)
{
    switch (command.type)
    {
    case CommandType::Submit:
        m_renderer->submit(command.batch);
        break;
    case CommandType::LoadClut:
        m_renderer->loadClut(command.tex0, command.texclut);
        break;
    case CommandType::BeginTransfer:
        m_renderer->beginTransfer(command.transfer);
        break;
    case CommandType::UploadImage:
        m_renderer->uploadImage(command.data.data(), static_cast<uint32_t>(command.data.size()));
        break;
    case CommandType::TextureFlush:
        m_renderer->textureFlush();
        break;
    case CommandType::Call:
        command.call();
        break;
    }
}

void GSGpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    runSync([&]()
            { m_renderer->initialize(vram, vramSize); });
}

void GSGpuBackend::Reset()
{
    runSync([&]()
            { m_renderer->reset(); });
}

void GSGpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    Command command;
    command.type = CommandType::Submit;
    command.batch = batch;
    enqueue(std::move(command));
}

void GSGpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    Command command;
    command.type = CommandType::LoadClut;
    command.tex0 = tex0;
    command.texclut = texclut;
    enqueue(std::move(command));
}

void GSGpuBackend::BeginTransfer(const GSTransferCommand &transfer)
{
    Command command;
    command.type = CommandType::BeginTransfer;
    command.transfer = transfer;
    enqueue(std::move(command));
}

void GSGpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Command command;
    command.type = CommandType::UploadImage;
    command.data.assign(data, data + sizeBytes);
    enqueue(std::move(command));
}

void GSGpuBackend::Flush()
{
}

void GSGpuBackend::TextureFlush()
{
    Command command;
    command.type = CommandType::TextureFlush;
    enqueue(std::move(command));
}

void GSGpuBackend::Sync(GSSyncReason reason)
{
    // FINISH/presentation are ordered in the stream; other reasons need the
    // render thread (and local memory) to be up to date.
    if (reason == GSSyncReason::Finish || reason == GSSyncReason::Presentation)
        return;
    runSync([&]()
            { m_renderer->syncLocalMemory(); });
}

PresentationFrame GSGpuBackend::Present(const GSPresentationRequest &request)
{
    PresentationFrame frame;
    runSync([&]()
            { frame = m_renderer->present(request); });
    return frame;
}

bool GSGpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    bool cleared = false;
    runSync([&]()
            { cleared = m_renderer->clearFramebuffer(context, rgba); });
    return cleared;
}

uint32_t GSGpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    uint32_t count = 0;
    runSync([&]()
            { count = m_renderer->consumeLocalToHostBytes(dst, maxBytes); });
    return count;
}

uint32_t GSGpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    uint32_t value = 0;
    runSync([&]()
            { value = m_renderer->readVram(psm, base, bw, x, y); });
    return value;
}

void GSGpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    runSync([&]()
            { m_renderer->writeVram(psm, base, bw, x, y, value); });
}

void GSGpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    runSync([&]()
            { m_renderer->snapshotVram(out); });
}

GSTransferSnapshot GSGpuBackend::GetTransferSnapshot() const
{
    GSTransferSnapshot snapshot{};
    runSync([&]()
            { snapshot = m_renderer->transferSnapshot(); });
    return snapshot;
}
