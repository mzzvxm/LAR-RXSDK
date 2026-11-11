/**
 * @file        net/posix_compat.h
 * @brief       POSIX shims for the Win32/Winsock names still used verbatim by
 *              the Xbox networking emulation (xam_net, mcla_online, xsocket).
 *
 * On Windows these come from <winsock2.h>/<ws2tcpip.h>; the socket layer was
 * only partially ported to Linux, so the handful of Windows spellings still
 * present in those translation units resolve here instead. Include this after
 * the POSIX socket headers.
 */
#pragma once

#include <rex/platform.h>

#if !REX_PLATFORM_WIN32

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <ctime>

// Win32 socket descriptor type; a plain fd on POSIX.
using SOCKET = int;

// Winsock keeps its own thread-local error; POSIX uses errno.
inline int WSAGetLastError() { return errno; }

// Winsock closes descriptors through a dedicated call; POSIX uses close().
inline int closesocket(int fd) { return ::close(fd); }

// Monotonic milliseconds since an arbitrary fixed point.
inline uint64_t GetTickCount64() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ull +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
}

// 32-bit wrapping variant, matching the Win32 original's overflow behavior.
inline uint32_t GetTickCount() { return static_cast<uint32_t>(GetTickCount64()); }

#endif  // !REX_PLATFORM_WIN32
