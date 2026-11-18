#include "flutter_window.h"

#include <optional>
#include <vector>

#include "flutter/generated_plugin_registrant.h"

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  // Test-application-only controls for repeatable real-window resize checks.
  test_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(), "media_kit_test/window",
          &flutter::StandardMethodCodec::GetInstance());
  test_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    if (call.method_name() == "resize") {
      const auto* size = std::get_if<flutter::EncodableList>(call.arguments());
      if (!size || size->size() != 2) {
        result->Error("size", "Expected [width, height]");
        return;
      }
      const auto width = std::get<int32_t>((*size)[0]);
      const auto height = std::get<int32_t>((*size)[1]);
      if (width < 320 || height < 240 || width > 3840 || height > 2160) {
        result->Error("size", "Invalid test window size");
        return;
      }
      if (!SetWindowPos(GetHandle(), nullptr, 0, 0, width, height,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        result->Error("resize", "SetWindowPos failed");
        return;
      }
      result->Success();
    } else if (call.method_name() == "fullscreen") {
      const bool enabled = std::get<bool>(*call.arguments());
      if (enabled != test_fullscreen_) {
        if (enabled) {
          test_window_style_ = GetWindowLongPtr(GetHandle(), GWL_STYLE);
          GetWindowPlacement(GetHandle(), &test_window_placement_);
          MONITORINFO monitor{sizeof(MONITORINFO)};
          GetMonitorInfo(
              MonitorFromWindow(GetHandle(), MONITOR_DEFAULTTONEAREST),
              &monitor);
          SetWindowLongPtr(GetHandle(), GWL_STYLE,
                           test_window_style_ & ~WS_OVERLAPPEDWINDOW);
          SetWindowPos(GetHandle(), nullptr, monitor.rcMonitor.left,
                       monitor.rcMonitor.top,
                       monitor.rcMonitor.right - monitor.rcMonitor.left,
                       monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                       SWP_NOZORDER | SWP_FRAMECHANGED);
        } else {
          SetWindowLongPtr(GetHandle(), GWL_STYLE, test_window_style_);
          SetWindowPlacement(GetHandle(), &test_window_placement_);
          SetWindowPos(
              GetHandle(), nullptr, 0, 0, 0, 0,
              SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        test_fullscreen_ = enabled;
      }
      result->Success();
    } else if (call.method_name() == "minimize" ||
               call.method_name() == "restore") {
      ShowWindow(GetHandle(),
                 call.method_name() == "minimize" ? SW_MINIMIZE : SW_RESTORE);
      result->Success();
    } else if (call.method_name() == "metrics") {
      DWORD handles = 0;
      GetProcessHandleCount(GetCurrentProcess(), &handles);
      const auto client = GetClientArea();
      result->Success(flutter::EncodableValue(flutter::EncodableMap{
          {flutter::EncodableValue("handles"),
           flutter::EncodableValue(static_cast<int64_t>(handles))},
          {flutter::EncodableValue("gdi"),
           flutter::EncodableValue(static_cast<int64_t>(
               GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)))},
          {flutter::EncodableValue("user"),
           flutter::EncodableValue(static_cast<int64_t>(
               GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)))},
          {flutter::EncodableValue("width"),
           flutter::EncodableValue(static_cast<int32_t>(client.right))},
          {flutter::EncodableValue("height"),
           flutter::EncodableValue(static_cast<int32_t>(client.bottom))},
          {flutter::EncodableValue("fullscreen"),
           flutter::EncodableValue(test_fullscreen_)},
          {flutter::EncodableValue("minimized"),
           flutter::EncodableValue(IsIconic(GetHandle()) != FALSE)},
      }));
    } else if (call.method_name() == "capture") {
      // Capture the composed client area, including the actual Flutter texture.
      // Player.screenshot() alone does not validate texture presentation or
      // fit.
      const auto client = GetClientArea();
      const int width = client.right, height = client.bottom;
      if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        result->Error("capture", "Invalid client size");
        return;
      }
      HDC source = GetDC(GetHandle());
      HDC memory = CreateCompatibleDC(source);
      BITMAPINFO info{};
      info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      info.bmiHeader.biWidth = width;
      info.bmiHeader.biHeight = -height;
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      void* pixels = nullptr;
      HBITMAP bitmap =
          CreateDIBSection(source, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
      HGDIOBJ previous =
          bitmap && memory ? SelectObject(memory, bitmap) : nullptr;
      const bool copied = previous && PrintWindow(GetHandle(), memory, 3);
      std::vector<uint8_t> bytes;
      if (copied) {
        const auto* begin = static_cast<const uint8_t*>(pixels);
        bytes.assign(begin, begin + static_cast<size_t>(width) * height * 4);
      }
      if (previous)
        SelectObject(memory, previous);
      if (bitmap)
        DeleteObject(bitmap);
      if (memory)
        DeleteDC(memory);
      ReleaseDC(GetHandle(), source);
      if (!copied) {
        result->Error("capture", "PrintWindow failed");
        return;
      }
      result->Success(flutter::EncodableValue(flutter::EncodableMap{
          {flutter::EncodableValue("width"), flutter::EncodableValue(width)},
          {flutter::EncodableValue("height"), flutter::EncodableValue(height)},
          {flutter::EncodableValue("bgra"), flutter::EncodableValue(bytes)},
      }));
    } else if (call.method_name() == "close") {
      result->Success();
      PostMessage(GetHandle(), WM_CLOSE, 0, 0);
    } else {
      result->NotImplemented();
    }
  });

  flutter_controller_->engine()->SetNextFrameCallback([&]() { this->Show(); });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  test_channel_.reset();
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd,
                              UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}
