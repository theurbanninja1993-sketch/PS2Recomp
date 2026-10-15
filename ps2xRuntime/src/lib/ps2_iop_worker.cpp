#include "ps2_iop_worker.h"

#include <algorithm>
#include <cstdlib>

namespace
{
    // The EE clock and one VBlank of it (294.912 MHz / 59.94 Hz), matching
    // ps2_runtime.cpp: the allowance is counted in EE cycles.
    constexpr uint64_t kEeCyclesPerVBlank = 4'920'120u;

    // How much IOP work to run between checks for posted requests. This is the
    // worst case a game-thread call waits: it posts, then blocks until the
    // worker reaches the queue, so a slice is a potential frame-time spike. A
    // quarter VBlank (~4 ms) is 24% of a frame at 60 fps - enough to be felt as
    // stutter even when the average rate is right - so keep it well under a
    // frame; a sixteenth is about 0.3 ms.
    constexpr uint64_t kSliceEeCycles = kEeCyclesPerVBlank / 16u;

    // How far the IOP may fall behind before its backlog is dropped. A long
    // stall (a load, a disc read) must not come back as a burst of catch-up
    // work; two VBlanks of slack is what the inline path allowed.
    constexpr uint64_t kMaxLagEeCycles = 2u * kEeCyclesPerVBlank;
}

IopWorker::IopWorker(ps2x::iop::IopSubsystem &iop)
    : m_iop(iop),
      // Off by default while it is being verified: PS2X_IOP_THREAD=1 enables it,
      // PS2X_IOP_THREAD=0 (or unset) keeps the inline path.
      m_enabled([]()
                {
                    const char *env = std::getenv("PS2X_IOP_THREAD");
                    return env != nullptr && std::atoi(env) != 0;
                }())
{
}

IopWorker::~IopWorker()
{
    stop();
}

void IopWorker::start()
{
    if (!m_enabled || m_running)
        return;
    m_stop = false;
    m_running = true;
    m_thread = std::thread([this]() { loop(); });
}

void IopWorker::stop()
{
    if (!m_thread.joinable())
    {
        m_running = false;
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    m_thread.join(); // the loop drains posted work before it exits
    m_running = false;
}

void IopWorker::run(const std::function<void()> &fn)
{
    if (!m_enabled || !m_running)
    {
        fn(); // not in use: run inline, exactly as before
        return;
    }
    std::unique_lock lock(m_mutex);
    const uint64_t ticket = ++m_posted;
    m_work.push_back(fn);
    m_wake.notify_one();
    m_done.wait(lock, [this, ticket]() { return m_completed >= ticket; });
}

void IopWorker::addAllowedEeCycles(uint64_t eeCycles)
{
    if (!m_enabled || !m_running)
        return;
    std::lock_guard lock(m_mutex);
    m_allowed += eeCycles;
    m_wake.notify_one();
}

void IopWorker::loop()
{
    std::unique_lock lock(m_mutex);
    // Posted work is the synchronous path - whoever posted it is waiting - so
    // drain it before leaving, even once stopping.
    while (!m_stop || !m_work.empty())
    {
        if (!m_work.empty())
        {
            std::function<void()> fn = std::move(m_work.front());
            m_work.pop_front();
            lock.unlock();
            fn();
            lock.lock();
            ++m_completed;
            m_done.notify_all();
            continue;
        }
        // Then the cycle allowance, in slices.
        const uint64_t iopEe = m_iop.cycles() * 8u;
        if (m_allowed > iopEe)
        {
            if (m_allowed - iopEe > kMaxLagEeCycles)
                m_allowed = iopEe + kMaxLagEeCycles; // drop a long stall's backlog
            const uint64_t give = std::min<uint64_t>(m_allowed - iopEe, kSliceEeCycles);
            lock.unlock();
            m_iop.runEeCycles(give);
            lock.lock();
            continue;
        }
        m_wake.wait(lock);
    }
}
