#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopMemory;

    class IopSysclib
    {
    public:
        explicit IopSysclib(IopMemory &memory) noexcept;

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu);

    private:
        [[nodiscard]] std::string formatGuestString(const std::string &format, const std::function<uint32_t()> &nextArg) const;

        IopMemory &m_memory;
    };
}
