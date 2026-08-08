// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#ifndef VIDEO_OUTPUT_MANAGER_H_
#define VIDEO_OUTPUT_MANAGER_H_

#include <flutter/plugin_registrar_windows.h>

#include <unordered_map>

#include "thread_pool.h"
#include "video_output.h"

// Creates and disposes |VideoOutput| instances for Flutter texture embedding.
//
// Public requests are synchronized before ANGLE, EGL and libmpv work is queued
// on the dedicated rendering worker.
class VideoOutputManager {
 public:
  VideoOutputManager(flutter::PluginRegistrarWindows* registrar);

  // Creates a new |VideoOutput|. The callback reports its initial Flutter
  // texture ID and later texture or dimension changes.
  void Create(
      int64_t handle,
      VideoOutputConfiguration configuration,
      std::function<void(int64_t, int64_t, int64_t)> texture_update_callback);

  // Sets the required video output size.
  // This forces |VideoOutput| to resize the internal OpenGL surface / D3D
  // texture.
  void SetSize(int64_t handle,
               std::optional<int64_t> width,
               std::optional<int64_t> height);

  // Destroys the |VideoOutput| with given handle.
  void Dispose(int64_t handle);

  ~VideoOutputManager();

 private:
  std::mutex mutex_ = std::mutex();
  // ANGLE EGL contexts and libmpv render contexts have thread affinity. A
  // single worker serializes their creation, rendering, resizing and cleanup
  // across every |VideoOutput| owned by this manager.
  //
  // Flutter invokes the registered texture callback on an engine thread. That
  // callback only copies or exposes the frame prepared by this worker; libmpv
  // rendering itself does not run in the callback.
  //
  // The following operations are performed through the |ThreadPool|:
  //
  // * Rendering a video frame with |mpv_render_context_render|, including the
  //   required EGL context binding, after being notified by
  //   |mpv_render_context_set_update_callback|.
  // * Creation and disposal of each |VideoOutput|.
  //     * For creation, |mpv_render_context_create| and construction of a new
  //       |ANGLESurfaceManager| are done through |ThreadPool| (in |VideoOutput|
  //       constructor).
  //     * For disposal, |ThreadPool| ensures that all the pending |Render| or
  //       |Resize| tasks finish before destroying |ANGLESurfaceManager|
  //       and |mpv_render_context|.
  // * Resizing |ANGLESurfaceManager| and creating newly sized Flutter textures
  //   (|flutter::GpuSurfaceTexture| or |flutter::PixelBufferTexture|).
  //
  // A single worker preserves ordering while mutexes continue to protect state
  // shared with Flutter texture callbacks and public manager requests.
  std::unique_ptr<ThreadPool> thread_pool_ = std::make_unique<ThreadPool>(1);
  flutter::PluginRegistrarWindows* registrar_ = nullptr;
  std::unordered_map<int64_t, std::unique_ptr<VideoOutput>> video_outputs_ = {};
};

#endif  // VIDEO_OUTPUT_MANAGER_H_
