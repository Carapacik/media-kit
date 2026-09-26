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

// Owns the ANGLE EGL context and D3D11 textures used by libmpv's OpenGL render
// API. libmpv draws into an EGL pbuffer backed by an internal shared D3D11
// texture. |Read| copies the completed frame into the separate texture exposed
// to Flutter through |handle|.
//
// This implementation originated from Flutter's former Windows ANGLE surface
// manager. Flutter keeps its current Windows EGL implementation here:
// https://github.com/flutter/flutter/tree/stable/engine/src/flutter/shell/platform/windows/egl

class ANGLESurfaceManager {
 public:
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  HANDLE handle() const { return handle_; }

  ANGLESurfaceManager(int32_t width, int32_t height);

  ~ANGLESurfaceManager();

  ANGLESurfaceManager(const ANGLESurfaceManager&) = delete;
  ANGLESurfaceManager& operator=(const ANGLESurfaceManager&) = delete;

  // Recreates the EGL surface and its backing D3D11 textures while preserving
  // the display and context.
  void SetSize(int32_t width, int32_t height);

  // Makes the EGL context current, invokes |callback| and waits for its OpenGL
  // work to finish before releasing the context.
  void Draw(std::function<void()> callback);

  // Copies the latest completed frame to the D3D11 texture exposed to Flutter.
  void Read();

  // Binds or unbinds this manager's EGL context on the calling thread.
  void MakeCurrent(bool value);

 private:
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
  HANDLE mutex_ = nullptr;
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
  static constexpr EGLint kD3D11FeatureLevel9_3DisplayAttributes[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
      EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE,
      9,
      EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE,
      3,
      EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
      EGL_TRUE,
      EGL_NONE,
  };
  static constexpr EGLint kD3D11FallbackDisplayAttributes[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
      EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
      EGL_TRUE,
      EGL_NONE,
  };

  // Number of active instances sharing ANGLE's process-wide EGL display.
  static int32_t instance_count_;
};

#endif  // ANGLE_SURFACE_MANAGER_H_
