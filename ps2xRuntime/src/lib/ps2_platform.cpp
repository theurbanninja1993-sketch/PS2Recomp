#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#endif

#include "runtime/ps2_platform.h"

#include <thread>
#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#endif
#include <cstdio>
#include "ThreadNaming.h"

#include <cstdlib>
#include <iostream>
#include <iterator>
#include <system_error>

std::filesystem::path ps2ExecutableDirectory()
{
    std::error_code ec;
#if defined(_WIN32)
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length != 0 && length < std::size(path))
        return std::filesystem::path(path).parent_path();
    return std::filesystem::current_path(ec);
#elif defined(__linux__)
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path(ec) : exe.parent_path();
#else
    return std::filesystem::current_path(ec);
#endif
}

namespace
{
    std::thread::id g_hostThread;
}

void ps2SetHostThread()
{
    g_hostThread = std::this_thread::get_id();
}

bool ps2IsHostThread()
{
    return g_hostThread == std::this_thread::get_id();
}

void ps2RaiseCurrentThreadPriority()
{
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#else
    // Lowering the nice value needs privileges; if it fails the thread simply
    // keeps the default, which is what happens today anyway.
    setpriority(PRIO_PROCESS, 0, -5);
#endif
}

size_t ps2ProcessResidentMb()
{
#if defined(_WIN32)
    // Resolved at run time: mingw's headers alias GetProcessMemoryInfo to the
    // K32 form, which links a static stub into the executable instead of
    // importing psapi, and a stub in a shipped binary is not worth the risk.
    using GetProcessMemoryInfoFn = BOOL(WINAPI *)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    static const GetProcessMemoryInfoFn s_get = []() -> GetProcessMemoryInfoFn
    {
        HMODULE psapi = LoadLibraryW(L"psapi.dll");
        return psapi != nullptr
                   ? reinterpret_cast<GetProcessMemoryInfoFn>(GetProcAddress(psapi, "GetProcessMemoryInfo"))
                   : nullptr;
    }();
    if (s_get != nullptr)
    {
        PROCESS_MEMORY_COUNTERS counters{};
        if (s_get(GetCurrentProcess(), &counters, sizeof(counters)))
            return counters.WorkingSetSize / (1024u * 1024u);
    }
    return 0u;
#else
    FILE *file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr)
        return 0u;
    long total = 0, resident = 0;
    const int read = std::fscanf(file, "%ld %ld", &total, &resident);
    std::fclose(file);
    if (read != 2)
        return 0u;
    const long pageKb = sysconf(_SC_PAGESIZE) / 1024;
    return static_cast<size_t>(resident) * static_cast<size_t>(pageKb > 0 ? pageKb : 4) / 1024u;
#endif
}

std::filesystem::path ps2ConfigDirectory()
{
    static const std::filesystem::path s_dir = []()
    {
        std::error_code ec;
        const bool portable = std::getenv("PS2X_PORTABLE") != nullptr ||
                              std::filesystem::exists(ps2ExecutableDirectory() / "portable.txt", ec);
        if (portable)
            return ps2ExecutableDirectory() / "config";
        if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            return std::filesystem::path(xdg) / "ps2recomp";
        if (const char *appData = std::getenv("APPDATA"); appData && *appData)
            return std::filesystem::path(appData) / "ps2recomp";
        if (const char *home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".config" / "ps2recomp";
        return std::filesystem::current_path() / "ps2recomp";
    }();
    std::error_code ec;
    std::filesystem::create_directories(s_dir, ec);
    return s_dir;
}

void ps2OpenFolder(const std::string &path)
{
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#if defined(__APPLE__)
    const std::string command = "open \"" + path + "\" &";
#else
    const std::string command = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
#endif
    if (std::system(command.c_str()) != 0)
        std::cerr << "could not open " << path << std::endl;
#endif
}

void ps2ShowFatalError(const std::string &message)
{
    std::cerr << message << std::endl;
#if defined(_WIN32)
    const int length = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(length, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, wide.data(), length);
    MessageBoxW(nullptr, wide.c_str(), L"PS2Recomp", MB_OK | MB_ICONERROR);
#endif
}

#if defined(_WIN32)
namespace ThreadNaming
{
    void SetCurrentThreadNameWin32(std::string_view name)
    {
        using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
        HMODULE kernel32 = ::GetModuleHandleW(L"Kernel32.dll");
        auto setThreadDescription =
            reinterpret_cast<SetThreadDescriptionFn>(reinterpret_cast<void *>(::GetProcAddress(kernel32, "SetThreadDescription")));
        if (setThreadDescription)
        {
            std::wstring wname(name.begin(), name.end());
            setThreadDescription(::GetCurrentThread(), wname.c_str());
        }
    }
}
#endif
