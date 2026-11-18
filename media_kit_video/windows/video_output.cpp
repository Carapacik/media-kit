// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "video_output.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

VideoOutput::VideoOutput(int64_t handle,
                         VideoOutputConfiguration configuration,
                         flutter::PluginRegistrarWindows* registrar,
                         ThreadPool* thread_pool_ref)
    : width_(configuration.width),
      height_(configuration.height),
      configuration_(configuration),
      handle_(reinterpret_cast<mpv_handle*>(handle)),
      registrar_(registrar),
      thread_pool_ref_(thread_pool_ref) {
  // EGL and libmpv render operations share one rendering worker across outputs.
  auto future = thread_pool_ref_->Post([this, configuration]() {
    try {
      mpv_set_option_string(handle_, "video-sync", "audio");
      mpv_set_option_string(handle_, "video-timing-offset", "0");
      if (configuration.enable_hardware_acceleration) {
        try {
          const auto size =
              media_kit::ResolveVideoFrameSize(1, 1, 0, width_, height_, false)
                  .value_or(media_kit::VideoFrameSize{1, 1});
          surface_manager_ =
              std::make_unique<ANGLESurfaceManager>(size.width, size.height);
          surface_manager_->MakeCurrent(true);
          mpv_opengl_init_params gl_init_params{
              [](auto, auto name) {
                return reinterpret_cast<void*>(eglGetProcAddress(name));
              },
              nullptr};
          mpv_render_param params[] = {
              {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
              {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
              {MPV_RENDER_PARAM_INVALID, nullptr}};
          const auto error =
              mpv_render_context_create(&render_context_, handle_, params);
          surface_manager_->MakeCurrent(false);
          if (error < 0)
            throw std::runtime_error(mpv_error_string(error));
        } catch (const std::exception& error) {
          std::cerr << "media_kit: ANGLE fallback: " << error.what()
                    << std::endl;
          if (surface_manager_)
            surface_manager_->MakeCurrent(false);
          surface_manager_.reset();
        }
      }
      software_ = !surface_manager_;
      if (software_) {
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_SW},
            {MPV_RENDER_PARAM_INVALID, nullptr}};
        const auto error =
            mpv_render_context_create(&render_context_, handle_, params);
        if (error < 0)
          throw std::runtime_error(mpv_error_string(error));
      }
      const auto size =
          media_kit::ResolveVideoFrameSize(1, 1, 0, width_, height_, software_)
              .value_or(media_kit::VideoFrameSize{1, 1});
      Resize(size.width, size.height);
      mpv_render_context_set_update_callback(
          render_context_,
          [](void* context) {
            static_cast<VideoOutput*>(context)->NotifyRender();
          },
          this);
      std::cout << "media_kit: VideoOutput: Using "
                << (software_ ? "S/W" : "H/W") << " rendering." << std::endl;
    } catch (...) {
      CleanUp();
      throw;
    }
  });
  // Propagate initialization errors instead of returning a partially built
  // output.
  future.get();
}

VideoOutput::~VideoOutput() {
  {
    std::lock_guard<std::mutex> lock(notification_mutex_);
    destroyed_ = true;
  }
  // All previously accepted tasks precede this barrier. No callback can enqueue
  // a new task after it; unregister callbacks never reference this object.
  thread_pool_ref_->Post([this]() { CleanUp(); }).get();
  for (auto& retirement : retirements_)
    retirement.get();
}

void VideoOutput::CleanUp() {
  if (render_context_) {
    mpv_render_context_set_update_callback(render_context_, nullptr, nullptr);
    if (surface_manager_)
      surface_manager_->MakeCurrent(true);
    mpv_render_context_free(render_context_);
    render_context_ = nullptr;
    if (surface_manager_)
      surface_manager_->MakeCurrent(false);
  }
  RetireTexture();
  surface_manager_.reset();
}

void VideoOutput::RetireTexture() {
  if (!texture_)
    return;
  auto retired = std::move(texture_);
  auto completion = std::make_shared<std::promise<void>>();
  retirements_.erase(
      std::remove_if(retirements_.begin(), retirements_.end(),
                     [](auto& future) {
                       return future.wait_for(std::chrono::seconds(0)) ==
                              std::future_status::ready;
                     }),
      retirements_.end());
  retirements_.push_back(completion->get_future());
  const auto id = retired->id;
  registrar_->texture_registrar()->UnregisterTexture(
      id, [retired = std::move(retired), completion]() mutable {
        // The callback, descriptors and COM references remain alive until the
        // engine has stopped requesting this particular registration.
        retired.reset();
        completion->set_value();
      });
}

void VideoOutput::NotifyRender() {
  std::lock_guard<std::mutex> lock(notification_mutex_);
  if (destroyed_)
    return;
  thread_pool_ref_->Post([this]() {
    if (destroyed_)
      return;
    try {
      CheckAndResize();
      Render();
    } catch (const std::exception& error) {
      std::cerr << "media_kit: VideoOutput render failed: " << error.what()
                << std::endl;
    }
  });
}

void VideoOutput::Render() {
  if (!texture_ || !render_context_)
    return;
  const auto& frame = texture_->frame;
  if (surface_manager_) {
    surface_manager_->Draw([&]() {
      mpv_opengl_fbo fbo{0, frame->size.width, frame->size.height, 0};
      mpv_render_param params[] = {{MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
                                   {MPV_RENDER_PARAM_INVALID, nullptr}};
      mpv_render_context_render(render_context_, params);
    });
  } else {
    // Flutter releases this lock only after uploading the returned pixel data.
    std::lock_guard<std::mutex> lock(frame->pixels_mutex);
    const auto layout =
        media_kit::GetSoftwareFrameLayout(frame->size, frame->pixels.size());
    if (!layout)
      throw std::runtime_error("Invalid software frame capacity");
    int size[]{frame->size.width, frame->size.height};
    size_t stride = layout->stride;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, "rgb0"},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, frame->pixels.data()},
        {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context_render(render_context_, params);
  }
  registrar_->texture_registrar()->MarkTextureFrameAvailable(texture_->id);
}

void VideoOutput::SetTextureUpdateCallback(
    std::function<void(int64_t, int64_t, int64_t)> callback) {
  thread_pool_ref_
      ->Post([this, callback = std::move(callback)]() {
        texture_update_callback_ = callback;
        const auto size = GetVideoSize();
        texture_update_callback_(texture_->id, size ? size->width : 0,
                                 size ? size->height : 0);
      })
      .get();
}

void VideoOutput::SetSize(std::optional<int64_t> width,
                          std::optional<int64_t> height) {
  thread_pool_ref_->Post([this, width, height]() {
    if (destroyed_)
      return;
    width_ = width;
    height_ = height;
    // A paused player may never issue another update callback.
    try {
      CheckAndResize();
      Render();
    } catch (const std::exception& error) {
      std::cerr << "media_kit: VideoOutput resize failed: " << error.what()
                << std::endl;
    }
  });
}

void VideoOutput::CheckAndResize() {
  const auto size = GetVideoSize();
  if (!size)
    return;
  if (texture_ && texture_->frame->size.width == size->width &&
      texture_->frame->size.height == size->height)
    return;
  Resize(size->width, size->height);
}

void VideoOutput::Resize(int64_t required_width, int64_t required_height) {
  auto next = std::make_shared<TextureRegistration>();
  auto frame = std::make_shared<Frame>();
  frame->size = {static_cast<int32_t>(required_width),
                 static_cast<int32_t>(required_height)};
  next->frame = frame;
  if (surface_manager_) {
    surface_manager_->SetSize(frame->size.width, frame->size.height);
    frame->gpu = surface_manager_->GetTextureGeneration();
    auto& descriptor = frame->gpu_descriptor;
    descriptor.struct_size = sizeof(descriptor);
    descriptor.handle = frame->gpu->handle;
    descriptor.width = descriptor.visible_width = frame->size.width;
    descriptor.height = descriptor.visible_height = frame->size.height;
    descriptor.format = kFlutterDesktopPixelFormatBGRA8888;
    // Resource lifetime is tied to unregister, not the GPU import callback.
    // The latter does NOT signal that Flutter's GPU has finished sampling.
    next->variant =
        std::make_unique<flutter::TextureVariant>(flutter::GpuSurfaceTexture(
            kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle, [frame](auto, auto) {
              frame->gpu->Read();
              return &frame->gpu_descriptor;
            }));
  } else {
    const auto layout = media_kit::GetSoftwareFrameLayout(
        frame->size, media_kit::kSoftwareBufferCapacity);
    if (!layout)
      throw std::runtime_error("Invalid software texture size");
    frame->pixels.resize(layout->bytes);
    auto& descriptor = frame->pixel_descriptor;
    descriptor.buffer = frame->pixels.data();
    descriptor.width = frame->size.width;
    descriptor.height = frame->size.height;
    descriptor.release_context = frame.get();
    descriptor.release_callback = [](void* context) {
      static_cast<Frame*>(context)->pixels_mutex.unlock();
    };
    next->variant = std::make_unique<flutter::TextureVariant>(
        flutter::PixelBufferTexture([frame](auto, auto) {
          frame->pixels_mutex.lock();
          return &frame->pixel_descriptor;
        }));
  }
  // The callback is self-contained before registration can expose it to
  // Flutter.
  next->id =
      registrar_->texture_registrar()->RegisterTexture(next->variant.get());
  if (next->id < 0)
    throw std::runtime_error("Flutter texture registration failed");
  RetireTexture();
  texture_ = std::move(next);
  texture_update_callback_(texture_->id, required_width, required_height);
}
std::optional<media_kit::VideoFrameSize> VideoOutput::GetVideoSize() {
  int64_t dw = 0, dh = 0, rotate = 0;
  // One snapshot for both sides. A missing property is normal before open or
  // for audio-only media; never inspect an uninitialized mpv_node.
  mpv_node params{};
  if (mpv_get_property(handle_, "video-out-params", MPV_FORMAT_NODE, &params) >=
      0) {
    if (params.format == MPV_FORMAT_NODE_MAP && params.u.list != nullptr) {
      for (int32_t i = 0; i < params.u.list->num; ++i) {
        const char* key = params.u.list->keys[i];
        const auto& value = params.u.list->values[i];
        if (key != nullptr && value.format == MPV_FORMAT_INT64) {
          if (strcmp(key, "dw") == 0)
            dw = value.u.int64;
          if (strcmp(key, "dh") == 0)
            dh = value.u.int64;
          if (strcmp(key, "rotate") == 0)
            rotate = value.u.int64;
        }
      }
    }
    mpv_free_node_contents(&params);
  }
  return media_kit::ResolveVideoFrameSize(dw, dh, rotate, width_, height_,
                                          software_);
}
