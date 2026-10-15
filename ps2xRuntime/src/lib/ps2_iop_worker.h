#pragma once

#include "ps2x/iop/iop_subsystem.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

// Runs the IOP on its own thread.
//
// The emulated EE reaches the IOP through PS2IopTransport and a few runtime
// methods, and every one of those runs the IOP inline on the game thread - the
// same thread that runs the EE. That thread is saturated on the owner's
// machines (GPU under 40%, CPU at 25%) while other cores idle, and because the
// IOP clocks the SPU2 its audio production is at the mercy of that thread's
// scheduling.
//
// The worker owns the IopSubsystem outright: every IOP operation is posted here
// and, where a result is needed, waited for. The IOP's own state is then
// single-threaded again - just on this thread - so it needs no lock of its own.
// What is genuinely shared is RDRAM and the host-side tables, which is what the
// real hardware shares too.
//
// PS2X_IOP_THREAD=0 keeps the old inline behaviour, so this can be A/B'd and
// reverted with one variable.
class IopWorker
{
public:
    explicit IopWorker(ps2x::iop::IopSubsystem &iop);
    ~IopWorker();

    IopWorker(const IopWorker &) = delete;
    IopWorker &operator=(const IopWorker &) = delete;

    // Starts the thread; no-op when PS2X_IOP_THREAD=0.
    void start();
    void stop();

    // Whether work is actually dispatched to the thread.
    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }

    // Runs fn on the IOP thread and waits for it. Falls back to running inline
    // when the thread is not in use.
    void run(const std::function<void()> &fn);

    template <typename Fn>
    auto runValue(Fn &&fn) -> decltype(fn())
    {
        using Result = decltype(fn());
        if constexpr (std::is_void_v<Result>)
        {
            run(std::function<void()>(std::forward<Fn>(fn)));
        }
        else
        {
            std::optional<Result> result;
            run([&]() { result.emplace(fn()); });
            return std::move(*result);
        }
    }

    // The IOP may run this many EE cycles of its own work from now on (the IOP
    // runs at 1/8 the EE clock, so 8 EE cycles are one IOP cycle). Topped up
    // from the game thread by real elapsed time; see advanceIopVBlank.
    void addAllowedEeCycles(uint64_t eeCycles);

private:
    void loop();

    ps2x::iop::IopSubsystem &m_iop;
    const bool m_enabled;

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake; // work posted, or allowance added
    std::condition_variable m_done; // a posted item finished

    std::deque<std::function<void()>> m_work;
    uint64_t m_allowed = 0;    // EE cycles of IOP work permitted so far
    uint64_t m_posted = 0;     // work items posted
    uint64_t m_completed = 0;  // work items run
    bool m_running = false;
    bool m_stop = false;
};
