// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "include/media_kit_video/texture_gl.h"

#include "include/media_kit_video/gl_render_thread.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <atomic>
#include <new>

#define NUM_BUFFERS 3

using MediaKitEGLCreateImageKHRProc =
    EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer,
                    const EGLint*);
using MediaKitEGLDestroyImageKHRProc = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
using MediaKitGLEGLImageTargetTexture2DOESProc = void (*)(GLenum,
                                                          GLeglImageOES);
using MediaKitEGLCreateSyncKHRProc =
    EGLSyncKHR (*)(EGLDisplay, EGLenum, const EGLint*);
using MediaKitEGLDestroySyncKHRProc = EGLBoolean (*)(EGLDisplay, EGLSyncKHR);
using MediaKitEGLWaitSyncKHRProc = EGLint (*)(EGLDisplay, EGLSyncKHR, EGLint);
using MediaKitEGLClientWaitSyncKHRProc =
    EGLint (*)(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR);

static MediaKitEGLCreateImageKHRProc media_kit_egl_create_image_khr = nullptr;
static MediaKitEGLDestroyImageKHRProc media_kit_egl_destroy_image_khr =
    nullptr;
static MediaKitGLEGLImageTargetTexture2DOESProc
    media_kit_gl_egl_image_target_texture_2d_oes = nullptr;
static MediaKitEGLCreateSyncKHRProc media_kit_egl_create_sync_khr = nullptr;
static MediaKitEGLDestroySyncKHRProc media_kit_egl_destroy_sync_khr = nullptr;
static MediaKitEGLWaitSyncKHRProc media_kit_egl_wait_sync_khr = nullptr;
static MediaKitEGLClientWaitSyncKHRProc media_kit_egl_client_wait_sync_khr =
    nullptr;

typedef struct {
  guint32 fbo;
  guint32 texture;
  EGLImageKHR egl_image;
  guint32 flutter_texture;
  gboolean flutter_texture_valid;
  std::atomic<EGLSyncKHR> render_sync;
} RenderBuffer;

struct _TextureGL {
  FlTextureGL parent_instance;

  RenderBuffer buffers[NUM_BUFFERS];

  int back_index;
  int front_index;
  std::atomic<int> mailbox_state;

  guint32 current_width;
  guint32 current_height;
  gboolean buffers_initialized;
  gboolean initialization_posted;
  std::atomic<gboolean> resizing;
  std::atomic<gboolean> hardware_failed;
  guint32 dummy_texture;
  gint32 consecutive_render_errors;
  gboolean render_gl_error_logged;
  gboolean flutter_texture_error_logged;
  gboolean skip_frame_error_logged;
  gboolean sync_error_logged;
  gboolean wait_sync_error_logged;

  VideoOutput* video_output;
};

G_DEFINE_TYPE(TextureGL, texture_gl, fl_texture_gl_get_type())

gboolean texture_gl_init_egl_extensions(EGLDisplay egl_display) {
  if (egl_display == EGL_NO_DISPLAY) {
    return FALSE;
  }

  media_kit_egl_create_image_khr =
      reinterpret_cast<MediaKitEGLCreateImageKHRProc>(
          eglGetProcAddress("eglCreateImageKHR"));
  media_kit_egl_destroy_image_khr =
      reinterpret_cast<MediaKitEGLDestroyImageKHRProc>(
          eglGetProcAddress("eglDestroyImageKHR"));
  media_kit_gl_egl_image_target_texture_2d_oes =
      reinterpret_cast<MediaKitGLEGLImageTargetTexture2DOESProc>(
          eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  media_kit_egl_create_sync_khr =
      reinterpret_cast<MediaKitEGLCreateSyncKHRProc>(
          eglGetProcAddress("eglCreateSyncKHR"));
  media_kit_egl_destroy_sync_khr =
      reinterpret_cast<MediaKitEGLDestroySyncKHRProc>(
          eglGetProcAddress("eglDestroySyncKHR"));
  media_kit_egl_wait_sync_khr =
      reinterpret_cast<MediaKitEGLWaitSyncKHRProc>(
          eglGetProcAddress("eglWaitSyncKHR"));
  media_kit_egl_client_wait_sync_khr =
      reinterpret_cast<MediaKitEGLClientWaitSyncKHRProc>(
          eglGetProcAddress("eglClientWaitSyncKHR"));

  if (!epoxy_has_egl_extension(egl_display, "EGL_KHR_image_base") ||
      !epoxy_has_egl_extension(egl_display, "EGL_KHR_gl_texture_2D_image") ||
      !epoxy_has_egl_extension(egl_display, "EGL_KHR_fence_sync")) {
    return FALSE;
  }

  return media_kit_egl_create_image_khr != nullptr &&
         media_kit_egl_destroy_image_khr != nullptr &&
         media_kit_gl_egl_image_target_texture_2d_oes != nullptr &&
         media_kit_egl_create_sync_khr != nullptr &&
         media_kit_egl_destroy_sync_khr != nullptr &&
         media_kit_egl_client_wait_sync_khr != nullptr;
}

static gboolean texture_gl_fail(TextureGL* self, const char* message) {
  if (self == nullptr) {
    g_printerr("media_kit: TextureGL: %s\n", message);
    return FALSE;
  }

  gboolean already_failed =
      self->hardware_failed.exchange(TRUE, std::memory_order_acq_rel);
  if (!already_failed) {
    g_printerr("media_kit: TextureGL: %s\n", message);
  }

  self->buffers_initialized = FALSE;
  self->resizing.store(FALSE, std::memory_order_release);
  return FALSE;
}

static gboolean texture_gl_skip_frame(TextureGL* self, const char* message) {
  if (self != nullptr && !self->skip_frame_error_logged) {
    g_printerr("media_kit: TextureGL: %s\n", message);
    self->skip_frame_error_logged = TRUE;
  }
  return FALSE;
}

struct SavedTextureState {
  GLint active_texture;
  GLint texture_binding_2d;
};

static SavedTextureState save_texture_state() {
  SavedTextureState state{GL_TEXTURE0, 0};
  glGetIntegerv(GL_ACTIVE_TEXTURE, &state.active_texture);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.texture_binding_2d);
  return state;
}

static void restore_texture_state(const SavedTextureState& state) {
  glBindTexture(GL_TEXTURE_2D, state.texture_binding_2d);
  glActiveTexture(state.active_texture);
}

static void clear_gl_errors() {
  while (glGetError() != GL_NO_ERROR) {
  }
}

static gboolean texture_gl_return_dummy_texture(TextureGL* self,
                                                guint32* target,
                                                guint32* name,
                                                guint32* width,
                                                guint32* height) {
  if (self == nullptr || target == nullptr || name == nullptr ||
      width == nullptr || height == nullptr) {
    return FALSE;
  }

  if (self->dummy_texture == 0) {
    glGenTextures(1, &self->dummy_texture);
    if (self->dummy_texture == 0) {
      return FALSE;
    }

    SavedTextureState saved_state = save_texture_state();
    glBindTexture(GL_TEXTURE_2D, self->dummy_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const guint32 pixel = 0x00000000;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, &pixel);
    restore_texture_state(saved_state);
  }

  *target = GL_TEXTURE_2D;
  *name = self->dummy_texture;
  *width = 1;
  *height = 1;
  return TRUE;
}

static void clear_render_buffer(EGLDisplay egl_display, RenderBuffer* buf) {
  if (buf == nullptr) {
    return;
  }

  EGLSyncKHR sync =
      buf->render_sync.exchange(EGL_NO_SYNC_KHR, std::memory_order_acq_rel);
  if (sync != EGL_NO_SYNC_KHR && egl_display != EGL_NO_DISPLAY &&
      media_kit_egl_destroy_sync_khr != nullptr) {
    media_kit_egl_destroy_sync_khr(egl_display, sync);
  }

  if (buf->egl_image != EGL_NO_IMAGE_KHR) {
    if (egl_display != EGL_NO_DISPLAY &&
        media_kit_egl_destroy_image_khr != nullptr) {
      media_kit_egl_destroy_image_khr(egl_display, buf->egl_image);
    }
    buf->egl_image = EGL_NO_IMAGE_KHR;
  }

  if (buf->texture != 0) {
    glDeleteTextures(1, &buf->texture);
    buf->texture = 0;
  }

  if (buf->fbo != 0) {
    glDeleteFramebuffers(1, &buf->fbo);
    buf->fbo = 0;
  }

  buf->flutter_texture_valid = FALSE;
}

static void clear_flutter_texture(RenderBuffer* buf) {
  if (buf == nullptr) {
    return;
  }

  if (buf->flutter_texture != 0) {
    glDeleteTextures(1, &buf->flutter_texture);
    buf->flutter_texture = 0;
  }

  buf->flutter_texture_valid = FALSE;
}

static gboolean create_render_buffer(RenderBuffer* buf,
                                     EGLDisplay egl_display,
                                     EGLContext egl_context,
                                     guint32 width,
                                     guint32 height) {
  if (buf == nullptr || egl_display == EGL_NO_DISPLAY ||
      egl_context == EGL_NO_CONTEXT || width < 1 || height < 1 ||
      media_kit_egl_create_image_khr == nullptr) {
    return FALSE;
  }

  glGenFramebuffers(1, &buf->fbo);
  if (buf->fbo == 0) {
    g_printerr("media_kit: TextureGL: Failed to create FBO.\n");
    return FALSE;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo);

  glGenTextures(1, &buf->texture);
  if (buf->texture == 0) {
    g_printerr("media_kit: TextureGL: Failed to create GL texture.\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return FALSE;
  }

  glBindTexture(GL_TEXTURE_2D, buf->texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  clear_gl_errors();
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);

  GLenum gl_error = glGetError();
  if (gl_error != GL_NO_ERROR) {
    g_printerr(
        "media_kit: TextureGL: glTexImage2D failed. Error: 0x%x\n",
        gl_error);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    return FALSE;
  }

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         buf->texture, 0);

  GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
    g_printerr("media_kit: TextureGL: FBO incomplete. Status: 0x%x\n",
               framebuffer_status);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    return FALSE;
  }

  EGLint egl_image_attribs[] = {EGL_NONE};
  buf->egl_image = media_kit_egl_create_image_khr(
      egl_display, egl_context, EGL_GL_TEXTURE_2D_KHR,
      reinterpret_cast<EGLClientBuffer>(static_cast<guintptr>(buf->texture)),
      egl_image_attribs);

  if (buf->egl_image == EGL_NO_IMAGE_KHR) {
    g_printerr(
        "media_kit: TextureGL: eglCreateImageKHR failed. Error: 0x%x\n",
        eglGetError());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    return FALSE;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  buf->flutter_texture_valid = FALSE;
  buf->render_sync.store(EGL_NO_SYNC_KHR, std::memory_order_release);

  return TRUE;
}

static void texture_gl_init(TextureGL* self) {
  new (&self->mailbox_state) std::atomic<int>(2);
  new (&self->resizing) std::atomic<gboolean>(FALSE);
  new (&self->hardware_failed) std::atomic<gboolean>(FALSE);

  for (int i = 0; i < NUM_BUFFERS; i++) {
    new (&self->buffers[i].render_sync)
        std::atomic<EGLSyncKHR>(EGL_NO_SYNC_KHR);
    self->buffers[i].fbo = 0;
    self->buffers[i].texture = 0;
    self->buffers[i].egl_image = EGL_NO_IMAGE_KHR;
    self->buffers[i].flutter_texture = 0;
    self->buffers[i].flutter_texture_valid = FALSE;
    self->buffers[i].render_sync.store(EGL_NO_SYNC_KHR,
                                       std::memory_order_relaxed);
  }

  self->back_index = 0;
  self->front_index = 1;
  self->mailbox_state.store(2, std::memory_order_relaxed);

  self->current_width = 1;
  self->current_height = 1;
  self->buffers_initialized = FALSE;
  self->initialization_posted = FALSE;
  self->resizing.store(FALSE, std::memory_order_relaxed);
  self->hardware_failed.store(FALSE, std::memory_order_relaxed);
  self->dummy_texture = 0;
  self->consecutive_render_errors = 0;
  self->render_gl_error_logged = FALSE;
  self->flutter_texture_error_logged = FALSE;
  self->skip_frame_error_logged = FALSE;
  self->sync_error_logged = FALSE;
  self->wait_sync_error_logged = FALSE;
  self->video_output = NULL;
}

static void texture_gl_dispose(GObject* object) {
  TextureGL* self = TEXTURE_GL(object);
  self->hardware_failed.store(TRUE, std::memory_order_release);
  self->resizing.store(FALSE, std::memory_order_release);

  // Flutter-facing textures are created in Flutter's texture callback
  // context. During shutdown that context may already be unavailable, so skip
  // GL deletion instead of issuing deletes against an unrelated/no context.
  if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
    for (int i = 0; i < NUM_BUFFERS; i++) {
      clear_flutter_texture(&self->buffers[i]);
    }

    if (self->dummy_texture != 0) {
      glDeleteTextures(1, &self->dummy_texture);
      self->dummy_texture = 0;
    }
  } else {
    for (int i = 0; i < NUM_BUFFERS; i++) {
      self->buffers[i].flutter_texture = 0;
      self->buffers[i].flutter_texture_valid = FALSE;
    }
    self->dummy_texture = 0;
  }

  VideoOutput* video_output = self->video_output;
  GLRenderThread* gl_thread =
      video_output != NULL ? video_output_get_gl_render_thread(video_output)
                           : NULL;

  auto clear_buffers = [self, video_output]() {
    EGLDisplay egl_display =
        video_output != NULL ? video_output_get_egl_display(video_output)
                             : EGL_NO_DISPLAY;
    EGLContext egl_context =
        video_output != NULL ? video_output_get_egl_context(video_output)
                             : EGL_NO_CONTEXT;

    if (egl_display != EGL_NO_DISPLAY && egl_context != EGL_NO_CONTEXT) {
      if (!eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                          egl_context)) {
        g_printerr(
            "media_kit: TextureGL: eglMakeCurrent failed during dispose. "
            "Error: 0x%x\n",
            eglGetError());
      }
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
      clear_render_buffer(egl_display, &self->buffers[i]);
    }
  };

  if (gl_thread != NULL) {
    gl_thread->PostAndWait(clear_buffers);
  } else {
    clear_buffers();
  }

  self->current_width = 1;
  self->current_height = 1;
  self->buffers_initialized = FALSE;
  self->back_index = 0;
  self->front_index = 1;
  self->mailbox_state.store(2, std::memory_order_release);
  self->video_output = NULL;

  G_OBJECT_CLASS(texture_gl_parent_class)->dispose(object);
}

static void texture_gl_class_init(TextureGLClass* klass) {
  FL_TEXTURE_GL_CLASS(klass)->populate = texture_gl_populate_texture;
  G_OBJECT_CLASS(klass)->dispose = texture_gl_dispose;
}

TextureGL* texture_gl_new(VideoOutput* video_output) {
  TextureGL* self = TEXTURE_GL(g_object_new(texture_gl_get_type(), NULL));
  self->video_output = video_output;
  return self;
}

void texture_gl_check_and_resize(TextureGL* self,
                                 gint64 required_width,
                                 gint64 required_height) {
  if (self == nullptr || self->video_output == nullptr) {
    return;
  }

  if (self->hardware_failed.load(std::memory_order_acquire)) {
    return;
  }

  if (required_width < 1 || required_height < 1) {
    return;
  }

  gboolean first_frame = !self->buffers_initialized;
  gboolean resize = self->current_width != static_cast<guint32>(required_width) ||
                    self->current_height !=
                        static_cast<guint32>(required_height);

  if (!first_frame && !resize) {
    return;
  }

  self->resizing.store(TRUE, std::memory_order_release);

  VideoOutput* video_output = self->video_output;
  EGLDisplay egl_display = video_output_get_egl_display(video_output);
  EGLContext egl_context = video_output_get_egl_context(video_output);

  if (egl_display == EGL_NO_DISPLAY || egl_context == EGL_NO_CONTEXT) {
    texture_gl_fail(self, "Invalid EGL display or context during resize.");
    return;
  }

  if (!eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                      egl_context)) {
    g_printerr(
        "media_kit: TextureGL: eglMakeCurrent failed during resize. "
        "Error: 0x%x\n",
        eglGetError());
    texture_gl_fail(self, "Failed to make EGL context current during resize.");
    return;
  }

  for (int i = 0; i < NUM_BUFFERS; i++) {
    clear_render_buffer(egl_display, &self->buffers[i]);
  }

  gboolean success = TRUE;
  for (int i = 0; i < NUM_BUFFERS; i++) {
    if (!create_render_buffer(&self->buffers[i], egl_display, egl_context,
                              static_cast<guint32>(required_width),
                              static_cast<guint32>(required_height))) {
      success = FALSE;
      break;
    }
  }

  if (!success) {
    for (int i = 0; i < NUM_BUFFERS; i++) {
      clear_render_buffer(egl_display, &self->buffers[i]);
    }

    self->buffers_initialized = FALSE;
    self->back_index = 0;
    self->front_index = 1;
    self->mailbox_state.store(2, std::memory_order_release);
    texture_gl_fail(self, "Failed to initialize triple buffers.");
    return;
  }

  glFlush();

  self->back_index = 0;
  self->front_index = 1;
  self->mailbox_state.store(2, std::memory_order_release);

  self->current_width = static_cast<guint32>(required_width);
  self->current_height = static_cast<guint32>(required_height);
  self->buffers_initialized = TRUE;
  self->resizing.store(FALSE, std::memory_order_release);
  self->initialization_posted = TRUE;
}

gboolean texture_gl_render(TextureGL* self) {
  if (self == nullptr || self->video_output == nullptr) {
    return FALSE;
  }

  if (self->hardware_failed.load(std::memory_order_acquire)) {
    return FALSE;
  }

  if (!self->buffers_initialized) {
    return FALSE;
  }

  VideoOutput* video_output = self->video_output;
  EGLDisplay egl_display = video_output_get_egl_display(video_output);
  EGLContext egl_context = video_output_get_egl_context(video_output);
  mpv_render_context* render_context =
      video_output_get_render_context(video_output);

  if (egl_display == EGL_NO_DISPLAY || egl_context == EGL_NO_CONTEXT) {
    return texture_gl_fail(self,
                           "Invalid EGL display or context during render.");
  }

  if (render_context == nullptr) {
    return texture_gl_fail(self, "mpv render context is null during render.");
  }

  if (media_kit_egl_client_wait_sync_khr == nullptr ||
      media_kit_egl_create_sync_khr == nullptr ||
      media_kit_egl_destroy_sync_khr == nullptr) {
    return texture_gl_fail(self, "EGL sync functions are unavailable.");
  }

  if (self->back_index < 0 || self->back_index >= NUM_BUFFERS) {
    return texture_gl_fail(self, "Invalid back buffer index during render.");
  }

  RenderBuffer* back_buf = &self->buffers[self->back_index];

  if (back_buf->fbo == 0 || back_buf->texture == 0 ||
      back_buf->egl_image == EGL_NO_IMAGE_KHR) {
    return texture_gl_fail(self,
                           "Invalid back buffer resources during render.");
  }

  if (!eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                      egl_context)) {
    g_printerr(
        "media_kit: TextureGL: eglMakeCurrent failed during render. "
        "Error: 0x%x\n",
        eglGetError());
    return texture_gl_fail(self,
                           "Failed to make EGL context current during render.");
  }

  EGLSyncKHR old_sync =
      back_buf->render_sync.exchange(EGL_NO_SYNC_KHR,
                                     std::memory_order_acq_rel);
  if (old_sync != EGL_NO_SYNC_KHR) {
    media_kit_egl_client_wait_sync_khr(
        egl_display, old_sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
        EGL_FOREVER_KHR);
    media_kit_egl_destroy_sync_khr(egl_display, old_sync);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, back_buf->fbo);

  GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
    g_printerr(
        "media_kit: TextureGL: Render FBO incomplete. Status: 0x%x\n",
        framebuffer_status);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return texture_gl_fail(self, "Render framebuffer is incomplete.");
  }

  mpv_opengl_fbo fbo{
      static_cast<gint32>(back_buf->fbo),
      static_cast<gint32>(self->current_width),
      static_cast<gint32>(self->current_height),
      0,
  };

  int flip_y = 0;
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
      {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };

  clear_gl_errors();
  mpv_render_context_render(render_context, params);

  GLenum gl_error = glGetError();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  if (gl_error != GL_NO_ERROR) {
    if (!self->render_gl_error_logged) {
      g_printerr("media_kit: TextureGL: GL error after mpv render: 0x%x\n",
                 gl_error);
      self->render_gl_error_logged = TRUE;
    }

    self->consecutive_render_errors++;
    if (self->consecutive_render_errors >= 5) {
      return texture_gl_fail(self, "Too many consecutive GL render errors.");
    }

    return texture_gl_skip_frame(self,
                                 "Skipping frame after GL render error.");
  }

  self->consecutive_render_errors = 0;
  self->render_gl_error_logged = FALSE;
  self->skip_frame_error_logged = FALSE;

  glFlush();

  EGLSyncKHR new_sync =
      media_kit_egl_create_sync_khr(egl_display, EGL_SYNC_FENCE_KHR, nullptr);
  if (new_sync == EGL_NO_SYNC_KHR) {
    if (!self->sync_error_logged) {
      g_printerr(
          "media_kit: TextureGL: eglCreateSyncKHR failed. Error: 0x%x\n",
          eglGetError());
      self->sync_error_logged = TRUE;
    }
    return TRUE;
  }

  back_buf->render_sync.store(new_sync, std::memory_order_release);
  self->sync_error_logged = FALSE;
  return TRUE;
}

void texture_gl_swap_buffers(TextureGL* self) {
  if (self == nullptr) {
    return;
  }

  if (self->hardware_failed.load(std::memory_order_acquire)) {
    return;
  }

  if (self->back_index < 0 || self->back_index >= NUM_BUFFERS) {
    texture_gl_fail(self, "Invalid back buffer index during swap.");
    return;
  }

  int new_state = (1 << 8) | self->back_index;
  int old_state =
      self->mailbox_state.exchange(new_state, std::memory_order_acq_rel);

  int old_index = old_state & 0xFF;
  if (old_index < 0 || old_index >= NUM_BUFFERS) {
    texture_gl_fail(self, "Invalid mailbox buffer index during swap.");
    return;
  }

  self->back_index = old_index;
}

gboolean texture_gl_populate_texture(FlTextureGL* texture,
                                     guint32* target,
                                     guint32* name,
                                     guint32* width,
                                     guint32* height,
                                     GError** error) {
  if (texture == nullptr) {
    return FALSE;
  }

  if (target == nullptr || name == nullptr || width == nullptr ||
      height == nullptr) {
    return FALSE;
  }

  TextureGL* self = TEXTURE_GL(texture);
  if (self == nullptr) {
    return FALSE;
  }

  VideoOutput* video_output = self->video_output;
  if (video_output == nullptr) {
    return texture_gl_return_dummy_texture(self, target, name, width, height);
  }

  if (self->hardware_failed.load(std::memory_order_acquire)) {
    return texture_gl_return_dummy_texture(self, target, name, width, height);
  }

  if (!self->initialization_posted && !self->buffers_initialized) {
    gint64 required_width = video_output_get_width(video_output);
    gint64 required_height = video_output_get_height(video_output);
    GLRenderThread* gl_thread = video_output_get_gl_render_thread(video_output);

    if (required_width > 0 && required_height > 0 && gl_thread != NULL) {
      self->initialization_posted = TRUE;
      video_output_notify_render(video_output);
    }
  }

  if (self->resizing.load(std::memory_order_acquire)) {
    return texture_gl_return_dummy_texture(self, target, name, width, height);
  }

  if (!self->buffers_initialized) {
    return texture_gl_return_dummy_texture(self, target, name, width, height);
  }

  EGLDisplay egl_display = video_output_get_egl_display(video_output);
  if (egl_display == EGL_NO_DISPLAY) {
    texture_gl_fail(self, "Invalid EGL display during populate.");
    return texture_gl_return_dummy_texture(self, target, name, width, height);
  }

  int current_state = self->mailbox_state.load(std::memory_order_acquire);
  while (current_state & 0x100) {
    int mailbox_index = current_state & 0xFF;
    if (mailbox_index < 0 || mailbox_index >= NUM_BUFFERS ||
        self->front_index < 0 || self->front_index >= NUM_BUFFERS) {
      texture_gl_fail(self, "Invalid mailbox buffer index during populate.");
      return texture_gl_return_dummy_texture(self, target, name, width,
                                             height);
    }

    int new_state = self->front_index;
    if (self->mailbox_state.compare_exchange_weak(
            current_state, new_state, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      self->front_index = mailbox_index;
      break;
    }
  }

  if (self->front_index < 0 || self->front_index >= NUM_BUFFERS) {
    texture_gl_fail(self, "Invalid front buffer index during populate.");
    return texture_gl_return_dummy_texture(self, target, name, width, height);
  }

  RenderBuffer* front_buf = &self->buffers[self->front_index];
  if (front_buf->egl_image == EGL_NO_IMAGE_KHR) {
    return texture_gl_return_dummy_texture(self, target, name, width, height);
  }

  EGLSyncKHR sync =
      front_buf->render_sync.exchange(EGL_NO_SYNC_KHR,
                                      std::memory_order_acq_rel);
  if (sync != EGL_NO_SYNC_KHR) {
    EGLint wait_result = EGL_FALSE;
    if (media_kit_egl_wait_sync_khr != nullptr) {
      wait_result = media_kit_egl_wait_sync_khr(egl_display, sync, 0);
    } else if (media_kit_egl_client_wait_sync_khr != nullptr) {
      wait_result = media_kit_egl_client_wait_sync_khr(
          egl_display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
          EGL_FOREVER_KHR);
    }

    if (wait_result == EGL_FALSE && !self->wait_sync_error_logged) {
      g_printerr("media_kit: TextureGL: EGL sync wait failed. Error: 0x%x\n",
                 eglGetError());
      self->wait_sync_error_logged = TRUE;
    }

    if (media_kit_egl_destroy_sync_khr != nullptr) {
      media_kit_egl_destroy_sync_khr(egl_display, sync);
    }
  }

  if (!front_buf->flutter_texture_valid &&
      front_buf->egl_image != EGL_NO_IMAGE_KHR) {
    clear_flutter_texture(front_buf);

    glGenTextures(1, &front_buf->flutter_texture);
    if (front_buf->flutter_texture == 0) {
      front_buf->flutter_texture_valid = FALSE;
      return texture_gl_return_dummy_texture(self, target, name, width,
                                             height);
    }

    SavedTextureState saved_state = save_texture_state();

    glBindTexture(GL_TEXTURE_2D, front_buf->flutter_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    clear_gl_errors();
    if (media_kit_gl_egl_image_target_texture_2d_oes != nullptr) {
      media_kit_gl_egl_image_target_texture_2d_oes(GL_TEXTURE_2D,
                                                   front_buf->egl_image);
    }

    GLenum gl_error =
        media_kit_gl_egl_image_target_texture_2d_oes == nullptr
            ? GL_INVALID_OPERATION
            : glGetError();

    restore_texture_state(saved_state);

    if (gl_error != GL_NO_ERROR) {
      if (!self->flutter_texture_error_logged) {
        g_printerr(
            "media_kit: TextureGL: Failed to bind EGLImage to Flutter "
            "texture. Error: 0x%x\n",
            gl_error);
        self->flutter_texture_error_logged = TRUE;
      }
      clear_flutter_texture(front_buf);
      return texture_gl_return_dummy_texture(self, target, name, width,
                                             height);
    }

    front_buf->flutter_texture_valid = TRUE;
    self->flutter_texture_error_logged = FALSE;
    video_output_notify_texture_update(video_output);
  }

  if (!front_buf->flutter_texture_valid || front_buf->flutter_texture == 0) {
    return texture_gl_return_dummy_texture(self, target, name, width, height);
  }

  *target = GL_TEXTURE_2D;
  *name = front_buf->flutter_texture;
  *width = self->current_width;
  *height = self->current_height;
  return TRUE;
}
