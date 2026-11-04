#pragma once

#include <WinSock2.h>
#include <Windows.h>

namespace fc_plugins::direct_transport {

// Captures the caller's Win32 and Winsock errors across patch work or optional diagnostics.
struct AmbientErrors {
    DWORD win32{};
    int winsock{};

    // Takes one snapshot before entering code that can change either ambient error.
    static AmbientErrors capture() noexcept {
        return {.win32 = GetLastError(), .winsock = WSAGetLastError()};
    }

    // Restores both error domains in the order needed when Winsock shares the thread error slot.
    void restore() const noexcept {
        WSASetLastError(winsock);
        SetLastError(win32);
    }
};

} // namespace fc_plugins::direct_transport
