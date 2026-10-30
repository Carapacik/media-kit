// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#ifndef ANGLE_SURFACE_MANAGER_H_
#define ANGLE_SURFACE_MANAGER_H_

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <Windows.h>

#include <d3d11.h>
#include <wrl.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

// |ANGLESurfaceManager| provides an abstraction around ANGLE to easily draw
// OpenGL ES 2.0 content & read as D3D 11 texture using shared |HANDLE|.
// * |Draw|: Takes callback where OpenGL ES 2.0 calls can be made for rendering.
// * |TextureGeneration::Read|: Copies that generation's completed frame to its
//   shared texture. Old generations remain valid across resize and teardown.

// A large part of implementation is inspired from Flutter.
// https://github.com/flutter/engine/blob/master/shell/platform/windows/angle_surface_manager.h

class ANGLESurfaceManager {
 public:
  // A texture generation can outlive the EGL surface and its manager. Flutter
  // callbacks keep their own generation until asynchronous unregister finishes.
  struct TextureGeneration {
    int32_t width;
    int32_t height;
    HANDLE handle;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> destination;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    std::shared_ptr<std::mutex> mutex;
    void Read();
  };

  std::shared_ptr<TextureGeneration> GetTextureGeneration();
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  HANDLE handle() const { return handle_; }

  ANGLESurfaceManager(int32_t width, int32_t height);

  ~ANGLESurfaceManager();

  ANGLESurfaceManager(const ANGLESurfaceManager&) = delete;
  ANGLESurfaceManager& operator=(const ANGLESurfaceManager&) = delete;

  // Recreates the surface and shared textures, changing |handle| while
  // preserving the EGL display and context. Serialized with Draw and generation
  // Read.
  // The optional callback publishes the replacement. If publication fails,
  // retain the previous surface, handles and dimensions.
  void SetSize(
      int32_t width,
      int32_t height,
      std::function<void(std::shared_ptr<TextureGeneration>)> publish = {});

  // Invokes |callback| with the EGL context current, waits for its GPU work
  // to finish, then unbinds the context. Serialized with |Read|.
  void Draw(std::function<void()> callback);

  // Binds or unbinds the EGL context on the calling thread. The caller must
  // serialize context use and unbind it before another thread can bind it.
  void MakeCurrent(bool value);

 private:
  std::shared_ptr<TextureGeneration> GetTextureGenerationUnlocked();
  void FinishRendering();

  void Create();

  void CleanUp(bool release_context);

  bool CreateD3DTexture();

  bool CreateEGLDisplay();

  bool CreateAndBindEGLSurface();

  int32_t width_ = 1;
  int32_t height_ = 1;
  HANDLE internal_handle_ = nullptr;
  HANDLE handle_ = nullptr;

  // Sync |Draw| & |Read| calls.
  std::shared_ptr<std::mutex> mutex_ = std::make_shared<std::mutex>();
  // D3D 11
  ID3D11Device* d3d_11_device_ = nullptr;
  ID3D11DeviceContext* d3d_11_device_context_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> internal_d3d_11_texture_2D_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d_11_texture_2D_;
  // ANGLE
  EGLSurface surface_ = EGL_NO_SURFACE;
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext context_ = nullptr;
  EGLConfig config_ = nullptr;

  static constexpr EGLint kEGLConfigurationAttributes[] = {
      EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE,    8,
      EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 8, EGL_STENCIL_SIZE, 8,
      EGL_NONE,
  };
  static constexpr EGLint kEGLContextAttributes[] = {
      EGL_CONTEXT_CLIENT_VERSION,
      2,
      EGL_NONE,
  };
  static constexpr EGLint kD3D11DisplayAttributes[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
      EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
      EGL_TRUE,
      EGL_NONE,
  };

  // Number of active instances of ANGLESurfaceManager.
  static int32_t instance_count_;
};

#endif  // ANGLE_SURFACE_MANAGER_H_
