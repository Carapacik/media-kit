// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.
#include "media_kit_video_plugin.h"

#include <Windows.h>
#include <stdexcept>

namespace media_kit_video {

void MediaKitVideoPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto plugin = std::make_unique<MediaKitVideoPlugin>(registrar);
  registrar->AddPlugin(std::move(plugin));
}

MediaKitVideoPlugin::MediaKitVideoPlugin(
    flutter::PluginRegistrarWindows* registrar)
    : registrar_(registrar),
      video_output_manager_(std::make_unique<VideoOutputManager>(registrar)) {
  static constexpr wchar_t kTaskWindowClass[] = L"MediaKitVideoTaskWindow";
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = WindowProcDelegate;
  window_class.hInstance = GetModuleHandle(nullptr);
  window_class.lpszClassName = kTaskWindowClass;
  if (!RegisterClassW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    throw std::runtime_error("Unable to register media_kit task window.");
  }
  task_window_ =
      CreateWindowExW(0, kTaskWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                      nullptr, window_class.hInstance, this);
  if (!task_window_) {
    throw std::runtime_error("Unable to create media_kit task window.");
  }

  channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      registrar->messenger(), "com.alexmercerind/media_kit_video",
      &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([&](const auto& call, auto result) {
    HandleMethodCall(call, std::move(result));
  });
}

MediaKitVideoPlugin::~MediaKitVideoPlugin() {
  video_output_manager_.reset();
  if (task_window_) {
    DestroyWindow(task_window_);
    task_window_ = nullptr;
  }
}

void MediaKitVideoPlugin::RunOnMainThread(std::function<void()> task) {
  if (!task_window_) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(main_thread_tasks_mutex_);
    main_thread_tasks_.push(std::move(task));
  }

  ::PostMessage(task_window_, kMainThreadTaskMessage, 0, 0);
}

LRESULT CALLBACK MediaKitVideoPlugin::WindowProcDelegate(HWND hwnd,
                                                         UINT message,
                                                         WPARAM wParam,
                                                         LPARAM lParam) {
  if (message == WM_NCCREATE) {
    auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto instance = reinterpret_cast<MediaKitVideoPlugin*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == kMainThreadTaskMessage && instance) {
    instance->ProcessMainThreadTasks();
    return 0;
  }
  if (message == WM_NCDESTROY) {
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
  }

  return ::DefWindowProc(hwnd, message, wParam, lParam);
}

void MediaKitVideoPlugin::ProcessMainThreadTasks() {
  std::queue<std::function<void()>> tasks_to_execute;

  {
    std::lock_guard<std::mutex> lock(main_thread_tasks_mutex_);
    tasks_to_execute.swap(main_thread_tasks_);
  }

  while (!tasks_to_execute.empty()) {
    auto task = std::move(tasks_to_execute.front());
    tasks_to_execute.pop();

    try {
      task();
    } catch (...) {
    }
  }
}

void MediaKitVideoPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name().compare("VideoOutputManager.Create") == 0) {
    auto arguments = std::get<flutter::EncodableMap>(*method_call.arguments());
    auto handle =
        std::get<std::string>(arguments[flutter::EncodableValue("handle")]);
    auto configuration = std::get<flutter::EncodableMap>(
        arguments[flutter::EncodableValue("configuration")]);

    auto handle_value = std::stoll(handle);
    auto configuration_value = VideoOutputConfiguration{};

    auto configuration_width =
        std::get<std::string>(configuration[flutter::EncodableValue("width")]);
    auto configuration_height =
        std::get<std::string>(configuration[flutter::EncodableValue("height")]);
    auto configuration_enable_hardware_acceleration = std::get<bool>(
        configuration[flutter::EncodableValue("enableHardwareAcceleration")]);
    if (configuration_width.compare("null") != 0) {
      configuration_value.width =
          static_cast<int64_t>(std::stoll(configuration_width.c_str()));
    }
    if (configuration_height.compare("null") != 0) {
      configuration_value.height =
          static_cast<int64_t>(std::stoll(configuration_height.c_str()));
    }
    configuration_value.enable_hardware_acceleration =
        configuration_enable_hardware_acceleration;

    video_output_manager_->Create(
        handle_value, configuration_value,
        [this, handle = handle_value](auto id, auto width, auto height) {
          RunOnMainThread([=]() {
            channel_->InvokeMethod(
                "VideoOutput.Resize",
                std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
                    {
                        flutter::EncodableValue("handle"),
                        flutter::EncodableValue(handle),
                    },
                    {
                        flutter::EncodableValue("id"),
                        flutter::EncodableValue(id),
                    },
                    {
                        flutter::EncodableValue("rect"),
                        flutter::EncodableValue(flutter::EncodableMap{
                            {
                                flutter::EncodableValue("left"),
                                flutter::EncodableValue(0),
                            },
                            {
                                flutter::EncodableValue("top"),
                                flutter::EncodableValue(0),
                            },
                            {
                                flutter::EncodableValue("width"),
                                flutter::EncodableValue(width),
                            },
                            {
                                flutter::EncodableValue("height"),
                                flutter::EncodableValue(height),
                            },
                        }),
                    },
                }),
                nullptr);
          });
        });
    result->Success(flutter::EncodableValue(std::monostate{}));
  } else if (method_call.method_name().compare("VideoOutputManager.Dispose") ==
             0) {
    auto arguments = std::get<flutter::EncodableMap>(*method_call.arguments());
    auto handle =
        std::get<std::string>(arguments[flutter::EncodableValue("handle")]);
    auto handle_value = static_cast<int64_t>(std::stoll(handle.c_str()));
    video_output_manager_->Dispose(handle_value);
    result->Success(flutter::EncodableValue(std::monostate{}));
  } else if (method_call.method_name().compare("VideoOutputManager.SetSize") ==
             0) {
    auto arguments = std::get<flutter::EncodableMap>(*method_call.arguments());
    auto handle =
        std::get<std::string>(arguments[flutter::EncodableValue("handle")]);
    auto width =
        std::get<std::string>(arguments[flutter::EncodableValue("width")]);
    auto height =
        std::get<std::string>(arguments[flutter::EncodableValue("height")]);
    auto handle_value = static_cast<int64_t>(std::stoll(handle.c_str()));
    auto width_value = std::optional<int64_t>{};
    auto height_value = std::optional<int64_t>{};
    if (width.compare("null") != 0) {
      width_value = static_cast<int64_t>(std::stoll(width.c_str()));
    }
    if (height.compare("null") != 0) {
      height_value = static_cast<int64_t>(std::stoll(height.c_str()));
    }
    video_output_manager_->SetSize(handle_value, width_value, height_value);
    result->Success(flutter::EncodableValue(std::monostate{}));
  } else if (method_call.method_name() == "Utils.EnterNativeFullscreen" ||
             method_call.method_name() == "Utils.ExitNativeFullscreen") {
    // Calls without arguments retain the single-window behavior (view 0).
    flutter::FlutterViewId view_id = 0;
    const auto* arguments = method_call.arguments();
    if (arguments && !std::holds_alternative<std::monostate>(*arguments)) {
      const auto* map = std::get_if<flutter::EncodableMap>(arguments);
      if (!map) {
        result->Error("invalid_arguments", "Expected a map containing viewId.");
        return;
      }
      auto id = map->find(flutter::EncodableValue("viewId"));
      if (id == map->end() || (!std::holds_alternative<int32_t>(id->second) &&
                               !std::holds_alternative<int64_t>(id->second))) {
        result->Error("invalid_arguments", "viewId must be an integer.");
        return;
      }
      view_id = id->second.LongValue();
    }
    // Drop saved state for views closed while they were fullscreen.
    for (auto it = fullscreen_windows_.begin();
         it != fullscreen_windows_.end();) {
      if (!registrar_->GetViewById(it->first)) {
        it = fullscreen_windows_.erase(it);
      } else {
        ++it;
      }
    }
    auto view = registrar_->GetViewById(view_id);
    auto window =
        view ? ::GetAncestor(view->GetNativeWindow(), GA_ROOT) : nullptr;
    if (!window || !::IsWindow(window)) {
      result->Error("view_not_found",
                    "The requested Flutter view has no window.");
      return;
    }
    auto saved = fullscreen_windows_.find(view_id);
    if (method_call.method_name() == "Utils.EnterNativeFullscreen") {
      if (saved == fullscreen_windows_.end()) {
        Utils::FullscreenState state{};
        if (!Utils::EnterNativeFullscreen(window, state)) {
          result->Error("fullscreen_failed", "Unable to enter fullscreen.");
          return;
        }
        fullscreen_windows_.emplace(view_id, state);
      }
    } else if (saved != fullscreen_windows_.end()) {
      if (!Utils::ExitNativeFullscreen(window, saved->second)) {
        result->Error("fullscreen_failed", "Unable to restore the window.");
        return;
      }
      fullscreen_windows_.erase(saved);
    }
    result->Success(flutter::EncodableValue(std::monostate{}));
  } else {
    result->NotImplemented();
  }
}

}  // namespace media_kit_video
