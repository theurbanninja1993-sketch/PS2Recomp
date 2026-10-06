#include "Common.h"
#include "TTY.h"
#include "ps2_log.h"

namespace ps2_stubs
{
    void scePrintf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t format_addr = getRegU32(ctx, 4);
        const std::string formatOwned = readPs2CStringBounded(rdram, runtime, format_addr, 1024);
        if (format_addr == 0)
            return;
        std::string rendered = formatPs2StringWithArgs(rdram, ctx, runtime, formatOwned.c_str(), 1);
        if (rendered.size() > 2048)
            rendered.resize(2048);
        PS2_IF_AGRESSIVE_LOGS({
            const std::string logLine = sanitizeForLog(rendered);
            uint32_t count = 0;
            {
                std::lock_guard<std::mutex> lock(g_printfLogMutex);
                count = ++g_printfLogCount;
            }
            if (count <= kMaxPrintfLogs)
            {
                RUNTIME_LOG("PS2 scePrintf: " << logLine);
                RUNTIME_LOG(std::flush);
            }
            else if (count == kMaxPrintfLogs + 1)
            {
                std::cerr << "PS2 printf logging suppressed after " << kMaxPrintfLogs << " lines" << std::endl;
            }
        });
    }

    void sceResetttyinit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Re-initialize TTY state; nothing host-side to reset. Report success.
        setReturnS32(ctx, 0);
    }

    void sceTtyHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Installs a TTY event handler. We have no async TTY events; report success.
        setReturnS32(ctx, 0);
    }

    void sceTtyInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // TTY (debug terminal) init. Nothing host-side to set up; report success.
        setReturnS32(ctx, 0);
    }

    void sceTtyRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // int sceTtyRead(char *buffer, int size)
        // No host console input is wired up; report 0 bytes read.
        setReturnS32(ctx, 0);
    }

    void sceTtyWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // int sceTtyWrite(const char *buffer, int size)
        // Games route debug/diagnostic text here. Surface it in the host log.
        uint32_t buffer_addr = getRegU32(ctx, 4);
        int32_t size = static_cast<int32_t>(getRegU32(ctx, 5));

        if (buffer_addr != 0 && size > 0)
        {
            const uint32_t capped = static_cast<uint32_t>(std::min<int32_t>(size, 4096));
            std::string text = readPs2CStringBounded(rdram, runtime, buffer_addr, capped);
            if (!text.empty())
            {
                std::cerr << "[TTY] " << text;
                if (text.back() != '\n')
                    std::cerr << std::endl;
            }
        }

        setReturnS32(ctx, size);
    }
}
