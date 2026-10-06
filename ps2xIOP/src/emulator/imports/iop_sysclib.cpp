#include "iop_sysclib.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"

#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    IopSysclib::IopSysclib(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    // printf-style formatting of a guest format string; `nextArg` yields the
    // successive 32-bit argument words.
    std::string IopSysclib::formatGuestString(const std::string &format, const std::function<uint32_t()> &nextArg) const
    {
        std::string out;
        for (size_t i = 0; i < format.size(); ++i)
        {
            const char c = format[i];
            if (c != '%' || i + 1u >= format.size())
            {
                out.push_back(c);
                continue;
            }
            ++i;
            if (format[i] == '%')
            {
                out.push_back('%');
                continue;
            }
            bool leftAlign = false;
            bool zeroPad = false;
            bool plus = false;
            bool space = false;
            bool alternate = false;
            for (;; ++i)
            {
                if (i >= format.size())
                    return out;
                const char flag = format[i];
                if (flag == '-') leftAlign = true;
                else if (flag == '0') zeroPad = true;
                else if (flag == '+') plus = true;
                else if (flag == ' ') space = true;
                else if (flag == '#') alternate = true;
                else break;
            }
            int width = 0;
            if (format[i] == '*')
            {
                width = static_cast<int32_t>(nextArg());
                if (width < 0)
                {
                    leftAlign = true;
                    width = -width;
                }
                ++i;
            }
            while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])))
                width = width * 10 + (format[i++] - '0');
            int precision = -1;
            if (i < format.size() && format[i] == '.')
            {
                ++i;
                precision = 0;
                if (i < format.size() && format[i] == '*')
                {
                    precision = std::max(0, static_cast<int32_t>(nextArg()));
                    ++i;
                }
                while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])))
                    precision = precision * 10 + (format[i++] - '0');
            }
            int longCount = 0;
            bool shortArg = false;
            while (i < format.size() && (format[i] == 'l' || format[i] == 'h'))
            {
                if (format[i] == 'l') ++longCount;
                else shortArg = true;
                ++i;
            }
            if (i >= format.size())
                break;
            const char conversion = format[i];

            std::string body;
            std::string prefix;
            switch (conversion)
            {
            case 'd':
            case 'i':
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            case 'p':
            {
                uint64_t value = nextArg();
                if (longCount >= 2)
                    value |= static_cast<uint64_t>(nextArg()) << 32u;
                const bool isSigned = conversion == 'd' || conversion == 'i';
                bool negative = false;
                if (isSigned)
                {
                    int64_t signedValue = longCount >= 2 ? static_cast<int64_t>(value)
                                                          : static_cast<int64_t>(static_cast<int32_t>(value));
                    if (shortArg)
                        signedValue = static_cast<int16_t>(signedValue);
                    negative = signedValue < 0;
                    value = negative ? static_cast<uint64_t>(-signedValue) : static_cast<uint64_t>(signedValue);
                }
                else if (shortArg)
                {
                    value &= 0xFFFFu;
                }
                const unsigned base = (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16u : (conversion == 'o' ? 8u : 10u);
                const char *digits = conversion == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
                do
                {
                    body.insert(body.begin(), digits[value % base]);
                    value /= base;
                } while (value != 0u);
                if (precision >= 0)
                {
                    zeroPad = false;
                    while (static_cast<int>(body.size()) < precision)
                        body.insert(body.begin(), '0');
                }
                if (negative) prefix = "-";
                else if (isSigned && plus) prefix = "+";
                else if (isSigned && space) prefix = " ";
                if ((alternate && base == 16u) || conversion == 'p')
                    prefix += conversion == 'X' ? "0X" : "0x";
                break;
            }
            case 'c':
                body.push_back(static_cast<char>(nextArg()));
                break;
            case 's':
            {
                const uint32_t address = nextArg();
                body = address != 0u ? m_memory.readString(address, precision >= 0 ? static_cast<size_t>(precision) : 4096u)
                                     : std::string("(null)");
                break;
            }
            default:
                body.push_back('%');
                body.push_back(conversion);
                break;
            }

            const int length = static_cast<int>(prefix.size() + body.size());
            if (width > length)
            {
                const std::string pad(static_cast<size_t>(width - length), zeroPad && !leftAlign ? '0' : ' ');
                if (leftAlign)
                    out += prefix + body + pad;
                else if (zeroPad)
                    out += prefix + pad + body;
                else
                    out += pad + prefix + body;
            }
            else
            {
                out += prefix + body;
            }
        }
        return out;
    }

    bool IopSysclib::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        const auto compare = [&](uint32_t lhs, uint32_t rhs, uint32_t count) -> int32_t
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t left = m_memory.read8(lhs + i);
                const uint8_t right = m_memory.read8(rhs + i);
                if (left != right)
                    return static_cast<int32_t>(left) - static_cast<int32_t>(right);
            }
            return 0;
        };
        const auto copy = [&](uint32_t destination, uint32_t source, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
                m_memory.write8(destination + i, m_memory.read8(source + i));
        };
        const auto appendString = [&](uint32_t destination, uint32_t source, std::optional<uint32_t> maxAppend = std::nullopt)
        {
            uint32_t destinationOffset = 0;
            while (m_memory.read8(destination + destinationOffset) != 0u && destinationOffset < (1u << 20))
                ++destinationOffset;

            uint32_t sourceOffset = 0;
            while (sourceOffset < (1u << 20) && (!maxAppend || sourceOffset < *maxAppend))
            {
                const uint8_t character = m_memory.read8(source + sourceOffset);
                m_memory.write8(destination + destinationOffset + sourceOffset, character);
                ++sourceOffset;
                if (character == 0u)
                    return;
            }
            m_memory.write8(destination + destinationOffset + sourceOffset, 0u);
        };

        switch (ordinal)
        {
        case 4: // setjmp - enough for callers which only test the initial return.
            setV0(0);
            return true;
        case 5: // longjmp, TODO bc w can do it without the BIOS jmp_buf ABI.
            setV0(a1 == 0u ? 1u : a1);
            return true;
        case 6:
            setV0(static_cast<uint32_t>(std::toupper(static_cast<unsigned char>(a0))));
            return true;
        case 7:
            setV0(static_cast<uint32_t>(std::tolower(static_cast<unsigned char>(a0))));
            return true;
        case 8:
        case 9: // ctype table is optional for most IRXs.
            setV0(0);
            return true;
        case 10: // memchr
            for (uint32_t i = 0; i < a2; ++i)
            {
                if (m_memory.read8(a0 + i) == static_cast<uint8_t>(a1))
                {
                    setV0(a0 + i);
                    return true;
                }
            }
            setV0(0);
            return true;
        case 11:
            setV0(static_cast<uint32_t>(compare(a0, a1, a2)));
            return true;
        case 12:
            copy(a0, a1, a2);
            setV0(a0);
            return true;
        case 13:
        {
            std::vector<uint8_t> temporary(a2);
            for (uint32_t i = 0; i < a2; ++i)
                temporary[i] = m_memory.read8(a1 + i);
            (void)m_memory.writeRam(a0, temporary.data(), temporary.size());
            setV0(a0);
            return true;
        }
        case 14:
            for (uint32_t i = 0; i < a2; ++i)
                m_memory.write8(a0 + i, static_cast<uint8_t>(a1));
            setV0(a0);
            return true;
        case 15: // bcmp
            setV0(static_cast<uint32_t>(compare(a0, a1, a2)));
            return true;
        case 16: // bcopy(src,dst,n)
            copy(a1, a0, a2);
            setV0(0);
            return true;
        case 17:
            for (uint32_t i = 0; i < a1; ++i)
                m_memory.write8(a0 + i, 0u);
            setV0(0);
            return true;
        case 18: // prnt
            setV0(0);
            return true;
        case 19: // sprintf(buf, fmt, ...): varargs in a2, a3, then the stack (O32)
        case 42: // vsprintf(buf, fmt, va_list)
        {
            const std::string format = m_memory.readString(a1, 4096u);
            std::string out;
            if (ordinal == 19u)
            {
                uint32_t index = 0;
                const uint32_t stackArgs = cpu.gpr[29] + 16u;
                out = formatGuestString(format, [&]() -> uint32_t
                                        {
                                            const uint32_t i = index++;
                                            if (i == 0u)
                                                return a2;
                                            if (i == 1u)
                                                return cpu.gpr[7];
                                            return m_memory.read32(stackArgs + (i - 2u) * 4u); });
            }
            else
            {
                uint32_t cursor = a2;
                out = formatGuestString(format, [&]() -> uint32_t
                                        {
                                            const uint32_t value = m_memory.read32(cursor);
                                            cursor += 4u;
                                            return value; });
            }
            for (size_t i = 0; i <= out.size(); ++i)
                m_memory.write8(a0 + static_cast<uint32_t>(i), i < out.size() ? static_cast<uint8_t>(out[i]) : 0u);
            setV0(static_cast<uint32_t>(out.size()));
            return true;
        }
        case 20:
            appendString(a0, a1);
            setV0(a0);
            return true;
        case 21: // strchr
        case 25: // index
        {
            const uint8_t needle = static_cast<uint8_t>(a1);
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const uint8_t character = m_memory.read8(a0 + i);
                if (character == needle)
                {
                    setV0(a0 + i);
                    return true;
                }
                if (character == 0u)
                    break;
            }
            setV0(0);
            return true;
        }
        case 22: // strcmp
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const uint8_t left = m_memory.read8(a0 + i);
                const uint8_t right = m_memory.read8(a1 + i);
                if (left != right)
                {
                    setV0(static_cast<uint32_t>(static_cast<int32_t>(left) - static_cast<int32_t>(right)));
                    return true;
                }
                if (left == 0u)
                    break;
            }
            setV0(0);
            return true;
        case 23: // strcpy
        {
            uint32_t i = 0;
            for (;; ++i)
            {
                const uint8_t character = m_memory.read8(a1 + i);
                m_memory.write8(a0 + i, character);
                if (character == 0u)
                    break;
            }
            setV0(a0);
            return true;
        }
        case 24: // strcspn
        {
            const std::string reject = m_memory.readString(a1, 4096u);
            uint32_t count = 0;
            for (; count < (1u << 20); ++count)
            {
                const char character = static_cast<char>(m_memory.read8(a0 + count));
                if (character == 0 || reject.find(character) != std::string::npos)
                    break;
            }
            setV0(count);
            return true;
        }
        case 26: // rindex
        case 32: // strrchr
        {
            const uint8_t needle = static_cast<uint8_t>(a1);
            uint32_t found = 0u;
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const uint8_t character = m_memory.read8(a0 + i);
                if (character == needle)
                    found = a0 + i;
                if (character == 0u)
                    break;
            }
            setV0(found);
            return true;
        }
        case 27:
            setV0(static_cast<uint32_t>(m_memory.readString(a0, 1u << 20).size()));
            return true;
        case 28:
            appendString(a0, a1, a2);
            setV0(a0);
            return true;
        case 29: // strncmp
            for (uint32_t i = 0; i < a2; ++i)
            {
                const uint8_t left = m_memory.read8(a0 + i);
                const uint8_t right = m_memory.read8(a1 + i);
                if (left != right)
                {
                    setV0(static_cast<uint32_t>(static_cast<int32_t>(left) - static_cast<int32_t>(right)));
                    return true;
                }
                if (left == 0u)
                    break;
            }
            setV0(0);
            return true;
        case 30: // strncpy
        {
            bool ended = false;
            for (uint32_t i = 0; i < a2; ++i)
            {
                const uint8_t character = ended ? 0u : m_memory.read8(a1 + i);
                if (character == 0u)
                    ended = true;
                m_memory.write8(a0 + i, character);
            }
            setV0(a0);
            return true;
        }
        case 31: // strpbrk
        {
            const std::string accept = m_memory.readString(a1, 4096u);
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const char character = static_cast<char>(m_memory.read8(a0 + i));
                if (character == 0)
                    break;
                if (accept.find(character) != std::string::npos)
                {
                    setV0(a0 + i);
                    return true;
                }
            }
            setV0(0);
            return true;
        }
        case 33: // strspn
        {
            const std::string accept = m_memory.readString(a1, 4096u);
            uint32_t count = 0;
            for (; count < (1u << 20); ++count)
            {
                const char character = static_cast<char>(m_memory.read8(a0 + count));
                if (character == 0 || accept.find(character) == std::string::npos)
                    break;
            }
            setV0(count);
            return true;
        }
        case 34: // strstr
        {
            const std::string needle = m_memory.readString(a1, 4096u);
            if (needle.empty())
            {
                setV0(a0);
                return true;
            }
            const std::string haystack = m_memory.readString(a0, 1u << 20);
            const size_t position = haystack.find(needle);
            setV0(position == std::string::npos
                      ? 0u
                      : a0 + static_cast<uint32_t>(position));
            return true;
        }
        case 35: // strtok state is intentionally not shared across modules yet.
            setV0(0);
            return true;
        case 36:
        case 38: // strtol / strtoul
        {
            const std::string value = m_memory.readString(a0, 4096u);
            char *end = nullptr;
            const int base = static_cast<int>(a2);
            const unsigned long parsed = ordinal == 36
                                             ? static_cast<unsigned long>(std::strtol(value.c_str(), &end, base))
                                             : std::strtoul(value.c_str(), &end, base);
            if (a1 != 0u)
            {
                m_memory.write32(a1, a0 + static_cast<uint32_t>(end - value.c_str()));
            }
            setV0(static_cast<uint32_t>(parsed));
            return true;
        }
        case 37: // atob
            setV0(0);
            return true;
        case 40: // _wmemcopy, count is 32-bit words
            for (uint32_t i = 0; i < a2; ++i)
                m_memory.write32(a0 + i * 4u, m_memory.read32(a1 + i * 4u));
            setV0(a0);
            return true;
        case 41:
            for (uint32_t i = 0; i < a2; ++i)
                m_memory.write32(a0 + i * 4u, a1);
            setV0(a0);
            return true;
        case 43:
            setV0(0);
            return true;
        default:
            return false;
        }
    }
}
