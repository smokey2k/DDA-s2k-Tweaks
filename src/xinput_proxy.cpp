#include <Windows.h>
#include <Xinput.h>

namespace {
HMODULE system_xinput() {
    static HMODULE module = [] {
        wchar_t system_directory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH - 14) {
            return static_cast<HMODULE>(nullptr);
        }
        wcscat_s(system_directory, L"\\XInput1_4.dll");
        return LoadLibraryW(system_directory);
    }();
    return module;
}

template <typename Function>
Function system_export(const char* name) {
    const HMODULE module = system_xinput();
    return module ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
}
}

extern "C" DWORD WINAPI XInputGetState(DWORD user_index, XINPUT_STATE* state) {
    using Function = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
    const auto function = system_export<Function>("XInputGetState");
    return function ? function(user_index, state) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputSetState(DWORD user_index, XINPUT_VIBRATION* vibration) {
    using Function = DWORD (WINAPI*)(DWORD, XINPUT_VIBRATION*);
    const auto function = system_export<Function>("XInputSetState");
    return function ? function(user_index, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}
