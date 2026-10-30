// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#ifndef VIDEO_OUTPUT_H_
#define VIDEO_OUTPUT_H_

#include <atomic>
#include <optional>
#include <vector>

#include <client.h>
#include <render.h>
#include <render_gl.h>

#include <future>
#include <memory>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include "angle_surface_manager.h"
#include "thread_pool.h"
#include "video_frame_size.h"

typedef struct _VideoOutputConfiguration {
  std::optional<int64_t> width;
  std::optional<int64_t> height;
  // Prefers OpenGL/ANGLE rendering, with software rendering as fallback.
  // Hardware video decoding is configured separately through mpv.
  bool enable_hardware_acceleration;

  _VideoOutputConfiguration(std::optional<int64_t> width = std::nullopt,
                            std::optional<int64_t> height = std::nullopt,
                            bool enable_hardware_acceleration = true)
      : width(width),
        height(height),
        enable_hardware_acceleration(enable_hardware_acceleration) {}
} VideoOutputConfiguration;

class VideoOutput {
 public:
  // Initialization and destruction wait for work queued on |thread_pool_ref|;
  // neither may run on that worker. The pool must outlive this output.
  VideoOutput(int64_t handle,
              VideoOutputConfiguration configuration,
              flutter::PluginRegistrarWindows* registrar,
              ThreadPool* thread_pool_ref);

  ~VideoOutput();

  // Reports (texture ID, width, height) on the rendering worker. Marshal any
  // platform-channel work to Flutter's platform thread.
  void SetTextureUpdateCallback(
      std::function<void(int64_t, int64_t, int64_t)> callback);

  // Queues size overrides; nullopt uses the video's corresponding dimension.
  // Queues a redraw as well, so resizing works while paused.
  void SetSize(std::optional<int64_t> width, std::optional<int64_t> height);

 private:
  struct Frame {
    media_kit::VideoFrameSize size;
    std::shared_ptr<ANGLESurfaceManager::TextureGeneration> gpu;
    std::vector<uint8_t> pixels;
    std::mutex pixels_mutex;
    FlutterDesktopGpuSurfaceDescriptor gpu_descriptor{};
    FlutterDesktopPixelBuffer pixel_descriptor{};
  };
  struct TextureRegistration {
    int64_t id = -1;
    std::shared_ptr<Frame> frame;
    std::unique_ptr<flutter::TextureVariant> variant;
  };

  void RetireTexture();
  void CleanUp();
  void NotifyRender();

  void Render();

  void CheckAndResize();

  void Resize(int64_t required_width, int64_t required_height);

  std::optional<media_kit::VideoFrameSize> GetVideoSize();

  std::optional<int64_t> width_ = std::nullopt;
  std::optional<int64_t> height_ = std::nullopt;
  VideoOutputConfiguration configuration_ = VideoOutputConfiguration{};

  mpv_handle* handle_ = nullptr;
  mpv_render_context* render_context_ = nullptr;
  flutter::PluginRegistrarWindows* registrar_ = nullptr;
  ThreadPool* thread_pool_ref_ = nullptr;
  // Serializes accepting render notifications with the teardown barrier.
  std::mutex notification_mutex_;
  std::atomic<bool> destroyed_{false};
  std::unique_ptr<ANGLESurfaceManager> surface_manager_;
  bool software_ = false;
  std::shared_ptr<TextureRegistration> texture_;
  // Unregister callbacks own only their generation, never this output.
  std::vector<std::future<void>> retirements_;
  // Reports the initial texture and subsequent texture or dimension changes.
  std::function<void(int64_t, int64_t, int64_t)> texture_update_callback_ =
      [](int64_t, int64_t, int64_t) {};
};

#endif  // VIDEO_OUTPUT_H_
