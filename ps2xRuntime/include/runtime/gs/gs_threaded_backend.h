#pragma once

#include "runtime/gs/gs_backend.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Runs raster backends on worker threads. Every worker owns a backend over the
// shared GS local memory and replays one ordered command stream, drawing only
// its own 8-row strips (SetRowBand). Commands that write or read local memory
// as a whole (transfers, presentation, texture flushes) are exclusive: all
// workers meet at a barrier and worker 0 runs them alone. A textured draw or
// CLUT load whose source may have been rendered since the last barrier gets a
// barrier first, so render-to-texture sees finished pixels.
//
// Host-side queries (local->host readback, VRAM reads/writes, clears, resets,
// debug snapshots) wait for every worker to drain first. Presentation is
// captured in stream order and returned one vblank late instead of stalling.
class GSThreadedBackend final : public GSRasterBackend
{
public:
    using BackendFactory = std::function<std::unique_ptr<GSRasterBackend>()>;

    GSThreadedBackend(BackendFactory factory, uint32_t workerCount);
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
        TextureFlush,
        Present,
        Barrier,
    };

    struct Command
    {
        CommandType type = CommandType::Barrier;
        GSPrimitiveBatch batch{};
        GSTex0Reg tex0{};
        GSTexClutReg texclut{};
        GSTransferCommand transfer{};
        GSPresentationRequest presentRequest{};
        std::vector<uint8_t> data;
    };

    struct Range
    {
        uint32_t begin = 0;
        uint32_t end = 0;
    };

    static constexpr uint32_t kChunkSize = 1024u;
    struct Chunk
    {
        Command commands[kChunkSize];
        // UploadImage: 0 = unclaimed, 1 = being written, 2 = written.
        std::atomic<uint8_t> uploadState[kChunkSize]{};
    };

    struct Worker
    {
        std::unique_ptr<GSRasterBackend> backend;
        std::thread thread;
        std::atomic<uint64_t> position{0}; // commands fully processed
    };

    void enqueue(Command &&command);
    void noteDrawTargets(const GSDrawState &state);
    static bool overlaps(const std::vector<Range> &ranges, uint32_t beginByte, uint32_t endByte);
    void addRange(std::vector<Range> &ranges, uint32_t beginByte, uint32_t endByte);
    void logHazard(const char *cause, uint32_t beginByte, uint32_t endByte);
    void drain() const;
    void workerLoop(uint32_t index);
    void runExclusive(Command &command);
    uint64_t slowestPosition() const;
    Command &commandAt(uint64_t sequence);

    std::vector<std::unique_ptr<Worker>> m_workers;

    mutable std::mutex m_mutex;
    mutable std::condition_variable m_workAvailable; // producer -> workers
    mutable std::condition_variable m_progress;      // workers -> producer/drain
    std::deque<std::unique_ptr<Chunk>> m_chunks;
    uint64_t m_firstChunkSequence = 0; // sequence of m_chunks.front()->commands[0]
    uint64_t m_published = 0;          // commands visible to workers
    bool m_stop = false;
    uint32_t m_sleepingWorkers = 0; // workers blocked on m_workAvailable
    mutable std::atomic<uint32_t> m_progressWaiters{0}; // threads blocked on m_progress

    // Exclusive-command barrier (generation counting).
    std::mutex m_barrierMutex;
    std::condition_variable m_barrierCv;
    // Spin-then-block: arrivals/generation are atomics so short waits never
    // touch the futex; the mutex/condvar only back long waits.
    std::atomic<uint32_t> m_barrierArrived{0};
    std::atomic<uint64_t> m_barrierGeneration{0};

    // Producer-side records since the last barrier: frame/Z memory drawn, and
    // texture/CLUT memory read.
    std::vector<Range> m_dirtyTargets;
    std::vector<Range> m_readSources;
    void enqueueBarrier();

    // PS2X_GS_STATS: periodic command/barrier counters.
    bool m_statsEnabled = false;
    uint64_t m_statCounts[8]{};
    uint64_t m_statHazardBarriers = 0;
    uint64_t m_statHazardCause[3]{}; // textured draw, CLUT load, host->local transfer
    uint64_t m_statRangeMerges = 0;
    uint32_t m_statHazardSamples = 0;
    uint64_t m_statBackpressureWaits = 0;
    uint64_t m_statLastReport = 0;
    void reportStats();

    std::mutex m_presentMutex;
    PresentationFrame m_lastPresented{};
};
