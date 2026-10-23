// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "utils.h"

typedef LONG NTSTATUS, *PNTSTATUS;
#define STATUS_SUCCESS (0x00000000)

typedef NTSTATUS(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

bool Utils::EnterNativeFullscreen(HWND window, FullscreenState& state) {
  state.style = ::GetWindowLongPtr(window, GWL_STYLE);
  state.placement.length = sizeof(WINDOWPLACEMENT);
  auto monitor = MONITORINFO{};
  monitor.cbSize = sizeof(MONITORINFO);
  if (!::GetWindowPlacement(window, &state.placement) ||
      !::GetMonitorInfo(::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                        &monitor)) {
    return false;
  }
  ::SetWindowLongPtr(window, GWL_STYLE, state.style & ~WS_OVERLAPPEDWINDOW);
  if (!::SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left,
                      monitor.rcMonitor.top,
                      monitor.rcMonitor.right - monitor.rcMonitor.left,
                      monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                      SWP_NOOWNERZORDER | SWP_FRAMECHANGED)) {
    ExitNativeFullscreen(window, state);
    return false;
  }
  return true;
}

bool Utils::ExitNativeFullscreen(HWND window, const FullscreenState& state) {
  ::SetWindowLongPtr(window, GWL_STYLE, state.style);
  const bool restored = ::SetWindowPlacement(window, &state.placement) != FALSE;
  const bool refreshed =
      ::SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED) != FALSE;
  return restored && refreshed;
}

RTL_OSVERSIONINFOW Utils::GetWindowsVersion() {
  HMODULE handle = ::LoadLibraryW(L"ntdll.dll");
  RTL_OSVERSIONINFOW rtl_os_version_info = {0};
  rtl_os_version_info.dwBuildNumber = 0;
  rtl_os_version_info.dwOSVersionInfoSize = sizeof(rtl_os_version_info);
  if (handle) {
    RtlGetVersionPtr rtl_get_version_ptr = reinterpret_cast<RtlGetVersionPtr>(
        ::GetProcAddress(handle, "RtlGetVersion"));
    if (rtl_get_version_ptr != nullptr) {
      rtl_get_version_ptr(&rtl_os_version_info);
    }
    ::FreeLibrary(handle);
  }
  return rtl_os_version_info;
}

bool Utils::IsWindows10RTMOrGreater() {
  return GetWindowsVersion().dwBuildNumber >= 10240;
}
