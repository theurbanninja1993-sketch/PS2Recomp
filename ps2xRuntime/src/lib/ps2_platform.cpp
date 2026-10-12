#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "runtime/ps2_platform.h"
#include "ThreadNaming.h"

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
