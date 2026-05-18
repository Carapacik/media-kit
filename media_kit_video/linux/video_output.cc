// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "include/media_kit_video/video_output.h"

#include "include/media_kit_video/gl_render_thread.h"
#include "include/media_kit_video/texture_gl.h"
#include "include/media_kit_video/texture_sw.h"

#include <epoxy/egl.h>
#include <epoxy/glx.h>
#include <gdk/gdkwayland.h>
#include <gdk/gdkx.h>

#include <atomic>
#include <cstring>
#include <new>

struct _VideoOutput {
  GObject parent_instance;
  TextureGL* texture_gl;
  gboolean texture_gl_registered;
  EGLDisplay egl_display; /* EGL display for mpv rendering. */
  EGLConfig egl_config;   /* EGL config from Flutter. */
  EGLContext egl_context; /* Isolated EGL context. */
  EGLSurface egl_surface; /* Placeholder surface, if one is ever needed. */
  guint8* pixel_buffer;
  TextureSW* texture_sw;
  gboolean texture_sw_registered;
  GMutex mutex; /* Only used in S/W rendering. */
  GMutex lifecycle_mutex; /* Guards destroyed checks around queued callbacks. */
  mpv_handle* handle;
  mpv_render_context* render_context;
  gboolean hardware_render_context_active;
  gint64 width;
  gint64 height;
  VideoOutputConfiguration configuration;
  TextureUpdateCallback texture_update_callback;
  gpointer texture_update_callback_context;
  FlTextureRegistrar* texture_registrar;
  GLRenderThread* gl_render_thread;
  std::atomic<gboolean> destroyed;
};

G_DEFINE_TYPE(VideoOutput, video_output, G_TYPE_OBJECT)

using MediaKitEGLGetPlatformDisplayEXTProc =
    EGLDisplay (*)(EGLenum, void*, const EGLint*);
#ifdef EGL_VERSION_1_5
using MediaKitEGLPlatformAttrib = EGLAttrib;
#else
using MediaKitEGLPlatformAttrib = EGLint;
#endif
using MediaKitEGLGetPlatformDisplayProc =
    EGLDisplay (*)(EGLenum, void*, const MediaKitEGLPlatformAttrib*);

static EGLDisplay get_egl_platform_display(EGLenum platform,
                                           void* native_display) {
  MediaKitEGLGetPlatformDisplayEXTProc egl_get_platform_display_ext =
      reinterpret_cast<MediaKitEGLGetPlatformDisplayEXTProc>(
          eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (egl_get_platform_display_ext != nullptr) {
    EGLDisplay display =
        egl_get_platform_display_ext(platform, native_display, NULL);
    if (display != EGL_NO_DISPLAY) {
      return display;
    }
  }

  MediaKitEGLGetPlatformDisplayProc egl_get_platform_display =
      reinterpret_cast<MediaKitEGLGetPlatformDisplayProc>(
          eglGetProcAddress("eglGetPlatformDisplay"));

#ifdef EGL_VERSION_1_5
  if (egl_get_platform_display == nullptr) {
    egl_get_platform_display = eglGetPlatformDisplay;
  }
#endif

  if (egl_get_platform_display == nullptr) {
    return EGL_NO_DISPLAY;
  }

  return egl_get_platform_display(platform, native_display, NULL);
}

static EGLDisplay get_egl_display_from_gdk() {
  GdkDisplay* display = gdk_display_get_default();
  if (display == nullptr) {
    return EGL_NO_DISPLAY;
  }

#ifdef EGL_PLATFORM_WAYLAND_EXT
  if (GDK_IS_WAYLAND_DISPLAY(display)) {
    return get_egl_platform_display(
        EGL_PLATFORM_WAYLAND_EXT,
        gdk_wayland_display_get_wl_display(display));
  }
#endif

#ifdef EGL_PLATFORM_X11_EXT
  if (GDK_IS_X11_DISPLAY(display)) {
    return get_egl_platform_display(EGL_PLATFORM_X11_EXT,
                                    gdk_x11_display_get_xdisplay(display));
  }
#endif

  return EGL_NO_DISPLAY;
}

static gboolean choose_egl_config(EGLDisplay egl_display, EGLConfig* config) {
  if (egl_display == EGL_NO_DISPLAY || config == nullptr) {
    return FALSE;
  }

  const EGLint config_attribs[] = {
      EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES2_BIT,
      EGL_NONE,
  };

  EGLint num_configs = 0;
  return eglChooseConfig(egl_display, config_attribs, config, 1,
                         &num_configs) == EGL_TRUE &&
         num_configs > 0;
}

static gboolean video_output_is_destroyed(VideoOutput* self) {
  if (self == nullptr) {
    return TRUE;
  }

  return self->destroyed.load(std::memory_order_acquire);
}

static gboolean configure_hardware_egl(VideoOutput* self) {
  if (self == nullptr) {
    return FALSE;
  }

  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    g_printerr(
        "media_kit: VideoOutput: Failed to bind OpenGL ES API. Error: 0x%x\n",
        eglGetError());
    return FALSE;
  }

  EGLDisplay flutter_display = eglGetCurrentDisplay();
  EGLContext flutter_context = eglGetCurrentContext();

  if (flutter_display != EGL_NO_DISPLAY &&
      flutter_context != EGL_NO_CONTEXT) {
    EGLint config_id = 0;
    if (eglQueryContext(flutter_display, flutter_context, EGL_CONFIG_ID,
                        &config_id)) {
      EGLConfig config = NULL;
      EGLint num_configs = 0;
      EGLint config_attribs[] = {EGL_CONFIG_ID, config_id, EGL_NONE};

      if (eglChooseConfig(flutter_display, config_attribs, &config, 1,
                          &num_configs) == EGL_TRUE &&
          num_configs > 0) {
        self->egl_display = flutter_display;
        self->egl_config = config;
        return TRUE;
      }
    }

    g_printerr(
        "media_kit: VideoOutput: Failed to query current Flutter EGL config; "
        "trying GDK EGL display.\n");
  } else {
    g_print(
        "media_kit: VideoOutput: Flutter EGL context is not current; trying "
        "GDK EGL display.\n");
  }

  EGLDisplay egl_display = get_egl_display_from_gdk();
  if (egl_display == EGL_NO_DISPLAY) {
    g_printerr("media_kit: VideoOutput: Failed to get GDK EGL display.\n");
    return FALSE;
  }

  if (eglInitialize(egl_display, nullptr, nullptr) != EGL_TRUE) {
    g_printerr(
        "media_kit: VideoOutput: Failed to initialize GDK EGL display. "
        "Error: 0x%x\n",
        eglGetError());
    return FALSE;
  }

  EGLConfig config = NULL;
  if (!choose_egl_config(egl_display, &config)) {
    g_printerr("media_kit: VideoOutput: Failed to choose EGL config.\n");
    return FALSE;
  }

  self->egl_display = egl_display;
  self->egl_config = config;
  return TRUE;
}

static void clear_hardware_resources(VideoOutput* self) {
  if (self == nullptr) {
    return;
  }

  gboolean has_hardware_resources =
      self->texture_gl != NULL || self->texture_gl_registered ||
      self->hardware_render_context_active ||
      self->egl_context != EGL_NO_CONTEXT ||
      self->egl_surface != EGL_NO_SURFACE;

  if (!has_hardware_resources) {
    return;
  }

  gboolean clear_render_context = self->hardware_render_context_active;

  if (clear_render_context && self->render_context != NULL) {
    mpv_render_context_set_update_callback(self->render_context, NULL, NULL);
  }

  if (self->texture_gl_registered && self->texture_gl != NULL &&
      self->texture_registrar != NULL) {
    fl_texture_registrar_unregister_texture(self->texture_registrar,
                                            FL_TEXTURE(self->texture_gl));
    self->texture_gl_registered = FALSE;
  }

  if (self->texture_gl != NULL) {
    g_object_unref(self->texture_gl);
    self->texture_gl = NULL;
  }

  auto clear_context = [self, clear_render_context]() {
    if (clear_render_context && self->render_context != NULL) {
      if (self->egl_display != EGL_NO_DISPLAY &&
          self->egl_context != EGL_NO_CONTEXT) {
        if (!eglMakeCurrent(self->egl_display, EGL_NO_SURFACE,
                            EGL_NO_SURFACE, self->egl_context)) {
          g_printerr(
              "media_kit: VideoOutput: eglMakeCurrent failed during "
              "hardware cleanup. Error: 0x%x\n",
              eglGetError());
        }
      }
      mpv_render_context_free(self->render_context);
      self->render_context = NULL;
      self->hardware_render_context_active = FALSE;
    }

    if (self->egl_surface != EGL_NO_SURFACE) {
      eglDestroySurface(self->egl_display, self->egl_surface);
      self->egl_surface = EGL_NO_SURFACE;
    }

    if (self->egl_context != EGL_NO_CONTEXT) {
      eglDestroyContext(self->egl_display, self->egl_context);
      self->egl_context = EGL_NO_CONTEXT;
    }
  };

  if (self->gl_render_thread != NULL) {
    self->gl_render_thread->PostAndWait(clear_context);
  } else {
    clear_context();
  }

  self->texture_gl_registered = FALSE;
  self->texture_gl = NULL;
  if (clear_render_context) {
    self->render_context = NULL;
    self->hardware_render_context_active = FALSE;
  }
  self->egl_context = EGL_NO_CONTEXT;
  self->egl_surface = EGL_NO_SURFACE;
  self->egl_config = NULL;
}

static void clear_software_resources(VideoOutput* self) {
  if (self == nullptr) {
    return;
  }

  if (self->render_context != NULL) {
    mpv_render_context_set_update_callback(self->render_context, NULL, NULL);
    mpv_render_context_free(self->render_context);
    self->render_context = NULL;
  }
  self->hardware_render_context_active = FALSE;

  if (self->texture_sw_registered && self->texture_sw != NULL &&
      self->texture_registrar != NULL) {
    fl_texture_registrar_unregister_texture(self->texture_registrar,
                                            FL_TEXTURE(self->texture_sw));
    self->texture_sw_registered = FALSE;
  }

  if (self->pixel_buffer != NULL) {
    g_free(self->pixel_buffer);
    self->pixel_buffer = NULL;
  }

  if (self->texture_sw != NULL) {
    g_object_unref(self->texture_sw);
    self->texture_sw = NULL;
  }
}

static void video_output_dispose(GObject* object) {
  VideoOutput* self = VIDEO_OUTPUT(object);
  if (video_output_is_destroyed(self)) {
    G_OBJECT_CLASS(video_output_parent_class)->dispose(object);
    return;
  }

  g_mutex_lock(&self->lifecycle_mutex);
  gboolean was_destroyed =
      self->destroyed.exchange(TRUE, std::memory_order_acq_rel);
  g_mutex_unlock(&self->lifecycle_mutex);
  if (was_destroyed) {
    G_OBJECT_CLASS(video_output_parent_class)->dispose(object);
    return;
  }

  if (self->render_context != NULL) {
    mpv_render_context_set_update_callback(self->render_context, NULL, NULL);
  }

  gboolean has_hardware_lifecycle =
      self->texture_gl != NULL || self->texture_gl_registered ||
      self->hardware_render_context_active ||
      self->egl_context != EGL_NO_CONTEXT ||
      self->egl_surface != EGL_NO_SURFACE;

  if (has_hardware_lifecycle && self->gl_render_thread != NULL) {
    self->gl_render_thread->PostAndWait([]() {});
  }

  clear_hardware_resources(self);
  clear_software_resources(self);

  g_mutex_clear(&self->mutex);
  g_mutex_clear(&self->lifecycle_mutex);
  G_OBJECT_CLASS(video_output_parent_class)->dispose(object);
}

static void video_output_class_init(VideoOutputClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = video_output_dispose;
}

static void video_output_init(VideoOutput* self) {
  self->texture_gl = NULL;
  self->texture_gl_registered = FALSE;
  self->egl_display = EGL_NO_DISPLAY;
  self->egl_config = NULL;
  self->egl_context = EGL_NO_CONTEXT;
  self->egl_surface = EGL_NO_SURFACE;
  self->texture_sw = NULL;
  self->texture_sw_registered = FALSE;
  self->pixel_buffer = NULL;
  self->handle = NULL;
  self->render_context = NULL;
  self->hardware_render_context_active = FALSE;
  self->width = 0;
  self->height = 0;
  self->configuration = VideoOutputConfiguration{};
  self->texture_update_callback = NULL;
  self->texture_update_callback_context = NULL;
  self->texture_registrar = NULL;
  self->gl_render_thread = NULL;
  new (&self->destroyed) std::atomic<gboolean>(FALSE);
  g_mutex_init(&self->mutex);
  g_mutex_init(&self->lifecycle_mutex);
}

VideoOutput* video_output_new(FlTextureRegistrar* texture_registrar,
                              FlView* view,
                              gint64 handle,
                              VideoOutputConfiguration configuration,
                              GLRenderThread* gl_render_thread) {
  VideoOutput* self = VIDEO_OUTPUT(g_object_new(video_output_get_type(), NULL));
  self->texture_registrar = texture_registrar;
  self->gl_render_thread = gl_render_thread;
  self->handle = reinterpret_cast<mpv_handle*>(handle);
  self->width = configuration.width;
  self->height = configuration.height;
  self->configuration = configuration;

#ifndef MPV_RENDER_API_TYPE_SW
  if (!self->configuration.enable_hardware_acceleration) {
    g_printerr("media_kit: VideoOutput: S/W rendering is not supported.\n");
  }
  self->configuration.enable_hardware_acceleration = TRUE;
#endif

  mpv_set_option_string(self->handle, "video-sync", "audio");
  // Causes frame drops with `pulse` audio output. (SlotSun/dart_simple_live#42)
  // mpv_set_option_string(self->handle, "video-timing-offset", "0");

  gboolean hardware_acceleration_supported = FALSE;

  if (self->configuration.enable_hardware_acceleration) {
    g_print("media_kit: VideoOutput: H/W init started.\n");

    if (self->gl_render_thread == NULL) {
      g_printerr("media_kit: VideoOutput: GL render thread is unavailable.\n");
    } else if (!configure_hardware_egl(self)) {
      g_printerr("media_kit: VideoOutput: Failed to configure EGL for H/W.\n");
    } else if (!texture_gl_init_egl_extensions(self->egl_display)) {
      g_printerr(
          "media_kit: VideoOutput: Required EGLImage/EGLSync functions or "
          "extensions are unavailable.\n");
    } else {
      self->texture_gl = texture_gl_new(self);
      if (self->texture_gl != NULL &&
          fl_texture_registrar_register_texture(texture_registrar,
                                                FL_TEXTURE(self->texture_gl))) {
        self->texture_gl_registered = TRUE;
      } else {
        g_printerr(
            "media_kit: VideoOutput: Failed to register H/W texture.\n");
      }

      if (self->texture_gl_registered && self->egl_display != EGL_NO_DISPLAY &&
          self->egl_config != NULL) {
        self->gl_render_thread->PostAndWait(
            [self, &hardware_acceleration_supported]() {
              if (video_output_is_destroyed(self)) {
                return;
              }

              eglBindAPI(EGL_OPENGL_ES_API);

              const EGLint context_attribs[] = {
                  EGL_CONTEXT_CLIENT_VERSION,
                  2,
                  EGL_NONE,
              };

              self->egl_context = eglCreateContext(
                  self->egl_display, self->egl_config, EGL_NO_CONTEXT,
                  context_attribs);

              if (self->egl_context == EGL_NO_CONTEXT) {
                g_printerr(
                    "media_kit: VideoOutput: Failed to create isolated EGL "
                    "context. Error: 0x%x\n",
                    eglGetError());
                return;
              }

              if (!eglMakeCurrent(self->egl_display, EGL_NO_SURFACE,
                                  EGL_NO_SURFACE, self->egl_context)) {
                g_printerr(
                    "media_kit: VideoOutput: Failed to make isolated EGL "
                    "context current. Error: 0x%x\n",
                    eglGetError());
                return;
              }

              mpv_opengl_init_params gl_init_params{
                  [](auto, auto name) {
                    return reinterpret_cast<void*>(eglGetProcAddress(name));
                  },
                  NULL,
              };

              mpv_render_param params[] = {
                  {MPV_RENDER_PARAM_API_TYPE,
                   const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
                  {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS,
                   reinterpret_cast<void*>(&gl_init_params)},
                  {MPV_RENDER_PARAM_INVALID, NULL},
                  {MPV_RENDER_PARAM_INVALID, NULL},
              };

              GdkDisplay* display = gdk_display_get_default();
              if (GDK_IS_WAYLAND_DISPLAY(display)) {
                params[2].type = MPV_RENDER_PARAM_WL_DISPLAY;
                params[2].data = gdk_wayland_display_get_wl_display(display);
              } else if (GDK_IS_X11_DISPLAY(display)) {
                params[2].type = MPV_RENDER_PARAM_X11_DISPLAY;
                params[2].data = gdk_x11_display_get_xdisplay(display);
              }

              if (mpv_render_context_create(&self->render_context,
                                            self->handle, params) == 0) {
                self->hardware_render_context_active = TRUE;
                mpv_render_context_set_update_callback(
                    self->render_context,
                    [](void* data) {
                      VideoOutput* self = static_cast<VideoOutput*>(data);
                      if (self == nullptr) {
                        return;
                      }
                      video_output_notify_render(self);
                    },
                    self);
                hardware_acceleration_supported = TRUE;
                g_print(
                    "media_kit: VideoOutput: H/W rendering with isolated EGL "
                    "context in dedicated thread.\n");
              } else {
                g_printerr(
                    "media_kit: VideoOutput: Failed to create H/W "
                    "mpv_render_context.\n");
              }
            });
      }
    }

    if (!hardware_acceleration_supported) {
      g_printerr(
          "media_kit: VideoOutput: H/W init failed; falling back to S/W.\n");
      clear_hardware_resources(self);
    } else {
      g_print("media_kit: VideoOutput: H/W init success.\n");
    }
  }

#ifdef MPV_RENDER_API_TYPE_SW
  if (!hardware_acceleration_supported) {
    g_printerr("media_kit: VideoOutput: S/W rendering.\n");
    self->pixel_buffer = g_new0(guint8, SW_RENDERING_PIXEL_BUFFER_SIZE);
    self->texture_sw = texture_sw_new(self);
    if (self->texture_sw != NULL &&
        fl_texture_registrar_register_texture(texture_registrar,
                                              FL_TEXTURE(self->texture_sw))) {
      self->texture_sw_registered = TRUE;
      mpv_render_param params[] = {
          {MPV_RENDER_PARAM_API_TYPE,
           const_cast<char*>(MPV_RENDER_API_TYPE_SW)},
          {MPV_RENDER_PARAM_INVALID, NULL},
      };
      if (mpv_render_context_create(&self->render_context, self->handle,
                                    params) == 0) {
        mpv_render_context_set_update_callback(
            self->render_context,
            [](void* data) {
              VideoOutput* self = static_cast<VideoOutput*>(data);
              if (self == nullptr) {
                return;
              }

              g_object_ref(self);
              gdk_threads_add_idle(
                  [](gpointer data) -> gboolean {
                    VideoOutput* self = static_cast<VideoOutput*>(data);
                    if (video_output_is_destroyed(self) ||
                        self->render_context == NULL ||
                        self->pixel_buffer == NULL) {
                      if (self != nullptr) {
                        g_object_unref(self);
                      }
                      return FALSE;
                    }

                    g_mutex_lock(&self->mutex);
                    gint64 width = video_output_get_width(self);
                    gint64 height = video_output_get_height(self);
                    if (width > 0 && height > 0) {
                      gint32 size[]{static_cast<gint32>(width),
                                    static_cast<gint32>(height)};
                      gint32 pitch = 4 * static_cast<gint32>(width);
                      mpv_render_param params[]{
                          {MPV_RENDER_PARAM_SW_SIZE, size},
                          {MPV_RENDER_PARAM_SW_FORMAT,
                           const_cast<char*>("rgb0")},
                          {MPV_RENDER_PARAM_SW_STRIDE, &pitch},
                          {MPV_RENDER_PARAM_SW_POINTER, self->pixel_buffer},
                          {MPV_RENDER_PARAM_INVALID, NULL},
                      };
                      mpv_render_context_render(self->render_context, params);
                      if (self->texture_sw != NULL &&
                          self->texture_sw_registered) {
                        fl_texture_registrar_mark_texture_frame_available(
                            self->texture_registrar,
                            FL_TEXTURE(self->texture_sw));
                      }
                    }
                    g_mutex_unlock(&self->mutex);
                    g_object_unref(self);
                    return FALSE;
                  },
                  self);
            },
            self);
      } else {
        g_printerr(
            "media_kit: VideoOutput: Failed to create S/W "
            "mpv_render_context.\n");
        clear_software_resources(self);
      }
    } else {
      g_printerr("media_kit: VideoOutput: Failed to register S/W texture.\n");
      clear_software_resources(self);
    }
  }
#else
  if (!hardware_acceleration_supported) {
    g_printerr("media_kit: VideoOutput: No renderer is available.\n");
  }
#endif

  return self;
}

void video_output_set_texture_update_callback(
    VideoOutput* self,
    TextureUpdateCallback texture_update_callback,
    gpointer texture_update_callback_context) {
  if (video_output_is_destroyed(self)) {
    return;
  }

  self->texture_update_callback = texture_update_callback;
  self->texture_update_callback_context = texture_update_callback_context;

  if (self->texture_update_callback == NULL) {
    return;
  }

  gint64 texture_id = video_output_get_texture_id(self);
  if (texture_id < 0) {
    return;
  }

  if (self->width == 0 || self->height == 0) {
    self->texture_update_callback(texture_id, 1, 1,
                                  self->texture_update_callback_context);
  } else {
    self->texture_update_callback(texture_id, self->width, self->height,
                                  self->texture_update_callback_context);
  }
}

void video_output_set_size(VideoOutput* self, gint64 width, gint64 height) {
  if (video_output_is_destroyed(self)) {
    return;
  }

  if (self->texture_gl) {
    self->width = width;
    self->height = height;
  }

  if (self->texture_sw) {
    self->width = CLAMP(width, 0, SW_RENDERING_MAX_WIDTH);
    self->height = CLAMP(height, 0, SW_RENDERING_MAX_HEIGHT);
  }
}

mpv_render_context* video_output_get_render_context(VideoOutput* self) {
  return self != nullptr ? self->render_context : NULL;
}

EGLDisplay video_output_get_egl_display(VideoOutput* self) {
  return self != nullptr ? self->egl_display : EGL_NO_DISPLAY;
}

EGLContext video_output_get_egl_context(VideoOutput* self) {
  return self != nullptr ? self->egl_context : EGL_NO_CONTEXT;
}

EGLSurface video_output_get_egl_surface(VideoOutput* self) {
  return self != nullptr ? self->egl_surface : EGL_NO_SURFACE;
}

GLRenderThread* video_output_get_gl_render_thread(VideoOutput* self) {
  return self != nullptr ? self->gl_render_thread : NULL;
}

guint8* video_output_get_pixel_buffer(VideoOutput* self) {
  return self != nullptr ? self->pixel_buffer : NULL;
}

gint64 video_output_get_width(VideoOutput* self) {
  if (self == nullptr) {
    return 0;
  }

  if (self->width) {
    return self->width;
  }

  gint64 width = 0;
  gint64 height = 0;

  mpv_node params;
  if (self->handle == NULL ||
      mpv_get_property(self->handle, "video-out-params", MPV_FORMAT_NODE,
                       &params) < 0) {
    return 0;
  }

  int64_t dw = 0, dh = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP) {
    for (int32_t i = 0; i < params.u.list->num; i++) {
      char* key = params.u.list->keys[i];
      auto value = params.u.list->values[i];
      if (value.format == MPV_FORMAT_INT64) {
        if (strcmp(key, "dw") == 0) {
          dw = value.u.int64;
        }
        if (strcmp(key, "dh") == 0) {
          dh = value.u.int64;
        }
        if (strcmp(key, "rotate") == 0) {
          rotate = value.u.int64;
        }
      }
    }
  }
  mpv_free_node_contents(&params);

  width = rotate == 0 || rotate == 180 ? dw : dh;
  height = rotate == 0 || rotate == 180 ? dh : dw;

  if (self->texture_sw != NULL) {
    if (width >= SW_RENDERING_MAX_WIDTH) {
      return SW_RENDERING_MAX_WIDTH;
    }
    if (height >= SW_RENDERING_MAX_HEIGHT && height != 0) {
      return width / height * SW_RENDERING_MAX_HEIGHT;
    }
  }

  return width;
}

gint64 video_output_get_height(VideoOutput* self) {
  if (self == nullptr) {
    return 0;
  }

  if (self->height) {
    return self->height;
  }

  gint64 width = 0;
  gint64 height = 0;

  mpv_node params;
  if (self->handle == NULL ||
      mpv_get_property(self->handle, "video-out-params", MPV_FORMAT_NODE,
                       &params) < 0) {
    return 0;
  }

  int64_t dw = 0, dh = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP) {
    for (int32_t i = 0; i < params.u.list->num; i++) {
      char* key = params.u.list->keys[i];
      auto value = params.u.list->values[i];
      if (value.format == MPV_FORMAT_INT64) {
        if (strcmp(key, "dw") == 0) {
          dw = value.u.int64;
        }
        if (strcmp(key, "dh") == 0) {
          dh = value.u.int64;
        }
        if (strcmp(key, "rotate") == 0) {
          rotate = value.u.int64;
        }
      }
    }
  }
  mpv_free_node_contents(&params);

  width = rotate == 0 || rotate == 180 ? dw : dh;
  height = rotate == 0 || rotate == 180 ? dh : dw;

  if (self->texture_sw != NULL) {
    if (height >= SW_RENDERING_MAX_HEIGHT) {
      return SW_RENDERING_MAX_HEIGHT;
    }
    if (width >= SW_RENDERING_MAX_WIDTH && width != 0) {
      return height / width * SW_RENDERING_MAX_WIDTH;
    }
  }

  return height;
}

gint64 video_output_get_texture_id(VideoOutput* self) {
  if (self == nullptr) {
    return -1;
  }

  if (self->texture_gl) {
    return reinterpret_cast<gint64>(self->texture_gl);
  }

  if (self->texture_sw) {
    return reinterpret_cast<gint64>(self->texture_sw);
  }

  return -1;
}

void video_output_notify_texture_update(VideoOutput* self) {
  if (video_output_is_destroyed(self) ||
      self->texture_update_callback == NULL) {
    return;
  }

  gint64 id = video_output_get_texture_id(self);
  if (id < 0) {
    return;
  }

  gint64 width = video_output_get_width(self);
  gint64 height = video_output_get_height(self);
  gpointer context = self->texture_update_callback_context;
  self->texture_update_callback(id, width, height, context);
}

void video_output_notify_render(VideoOutput* self) {
  if (self == nullptr) {
    return;
  }

  g_mutex_lock(&self->lifecycle_mutex);
  if (self->destroyed.load(std::memory_order_acquire) ||
      self->gl_render_thread == NULL ||
      self->texture_gl == NULL || self->render_context == NULL) {
    g_mutex_unlock(&self->lifecycle_mutex);
    return;
  }

  bool posted = self->gl_render_thread->Post([self]() {
    if (!video_output_is_destroyed(self)) {
      video_output_check_and_resize(self);
    }

    if (!video_output_is_destroyed(self)) {
      video_output_render(self);
    }
  });
  g_mutex_unlock(&self->lifecycle_mutex);
  if (!posted) {
    return;
  }
}

void video_output_check_and_resize(VideoOutput* self) {
  if (video_output_is_destroyed(self) || self->texture_gl == NULL) {
    return;
  }

  gint64 required_width = video_output_get_width(self);
  gint64 required_height = video_output_get_height(self);

  if (required_width < 1 || required_height < 1) {
    return;
  }

  texture_gl_check_and_resize(self->texture_gl, required_width,
                              required_height);
}

void video_output_render(VideoOutput* self) {
  if (video_output_is_destroyed(self)) {
    return;
  }

  if (self->texture_gl && self->render_context) {
    gboolean rendered = texture_gl_render(self->texture_gl);
    if (!rendered) {
      return;
    }

    texture_gl_swap_buffers(self->texture_gl);

    if (self->texture_gl_registered) {
      fl_texture_registrar_mark_texture_frame_available(
          self->texture_registrar, FL_TEXTURE(self->texture_gl));
    }
  }
}
