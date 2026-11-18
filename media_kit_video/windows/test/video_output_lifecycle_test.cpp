// Exercise production output/manager code with injected mpv and registrar
// failures. ANGLE and D3D resources are real; no Flutter engine is required.
#include "../video_output_manager.h"

#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace std::chrono_literals;

void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

struct mpv_render_context {
  bool hardware;
  mpv_render_update_fn callback = nullptr;
  void* callback_context = nullptr;
};

bool fail_gl = false, fail_sw = false, fail_render = false;
int live_contexts = 0, gl_attempts = 0, sw_attempts = 0, wrong_surface = 0;
mpv_render_context* active_context = nullptr;

extern "C" {
int mpv_set_option_string(mpv_handle*, const char*, const char*) {
  return 0;
}
const char* mpv_error_string(int) {
  return "injected mpv failure";
}
int mpv_get_property(mpv_handle*, const char*, mpv_format, void*) {
  return -1;
}
void mpv_free_node_contents(mpv_node*) {}
int mpv_render_context_create(mpv_render_context** result,
                              mpv_handle*,
                              mpv_render_param* params) {
  const bool hardware = strcmp(static_cast<const char*>(params[0].data),
                               MPV_RENDER_API_TYPE_OPENGL) == 0;
  if (hardware) {
    ++gl_attempts;
    Check(eglGetCurrentContext() != EGL_NO_CONTEXT, "GL create needs context");
  } else {
    ++sw_attempts;
    Check(eglGetCurrentContext() == EGL_NO_CONTEXT,
          "fallback left EGL current");
  }
  if (hardware ? fail_gl : fail_sw)
    return -1;
  *result = active_context = new mpv_render_context{hardware};
  ++live_contexts;
  return 0;
}
void mpv_render_context_set_update_callback(mpv_render_context* ctx,
                                            mpv_render_update_fn callback,
                                            void* data) {
  ctx->callback = callback;
  ctx->callback_context = data;
}
int mpv_render_context_render(mpv_render_context* ctx,
                              mpv_render_param* params) {
  if (fail_render)
    return -1;
  if (ctx->hardware) {
    auto* fbo = static_cast<mpv_opengl_fbo*>(params[0].data);
    EGLint width = 0, height = 0;
    eglQuerySurface(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW),
                    EGL_WIDTH, &width);
    eglQuerySurface(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW),
                    EGL_HEIGHT, &height);
    if (width != fbo->w || height != fbo->h)
      ++wrong_surface;
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  return 0;
}
void mpv_render_context_free(mpv_render_context* ctx) {
  Check(ctx->callback == nullptr, "callback must be removed before free");
  if (ctx->hardware)
    Check(eglGetCurrentContext() != EGL_NO_CONTEXT, "GL free needs context");
  delete ctx;
  active_context = nullptr;
  --live_contexts;
}
}

class Registrar : public flutter::TextureRegistrar {
 public:
  std::atomic<bool> fail_next{false};
  std::atomic<bool> defer{false};
  std::atomic<int> marks{0};
  int64_t RegisterTexture(flutter::TextureVariant* texture) override {
    if (fail_next.exchange(false))
      return -1;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id = next_++;  // ID zero is valid, including for unregister.
    textures_[id] = texture;
    return id;
  }
  bool MarkTextureFrameAvailable(int64_t id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    Check(textures_.count(id) == 1, "mark for unknown texture");
    ++marks;
    return true;
  }
  void UnregisterTexture(int64_t id, std::function<void()> callback) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Check(textures_.count(id) == 1, "unregister for unknown texture");
      if (defer) {
        retired_.emplace_back(id, std::move(callback));
        condition_.notify_all();
        return;
      }
      textures_.erase(id);
    }
    callback();
  }
  bool UnregisterTexture(int64_t) override {
    throw std::runtime_error("legacy unregister");
  }
  void WaitForRetirement() {
    std::unique_lock<std::mutex> lock(mutex_);
    Check(condition_.wait_for(lock, 5s, [&] { return !retired_.empty(); }),
          "unregister was not requested");
  }
  void CompleteRetirements() {
    std::vector<std::pair<int64_t, std::function<void()>>> retired;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      retired.swap(retired_);
      for (auto& entry : retired)
        textures_.erase(entry.first);
    }
    for (auto& entry : retired)
      entry.second();
  }
  size_t Count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return textures_.size();
  }
  void Read(int64_t id, size_t width, size_t height) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* texture = textures_.at(id);
    if (auto* gpu = std::get_if<flutter::GpuSurfaceTexture>(texture)) {
      auto* descriptor = gpu->ObtainDescriptor(width, height);
      Check(descriptor->handle && descriptor->width == width &&
                descriptor->height == height,
            "invalid retained GPU descriptor");
    } else {
      auto* buffer =
          std::get<flutter::PixelBufferTexture>(*texture).CopyPixelBuffer(
              width, height);
      Check(
          buffer->buffer && buffer->width == width && buffer->height == height,
          "invalid retained software descriptor");
      buffer->release_callback(buffer->release_context);
    }
  }

 private:
  int64_t next_ = 0;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::map<int64_t, flutter::TextureVariant*> textures_;
  std::vector<std::pair<int64_t, std::function<void()>>> retired_;
};

void TestFailures(bool hardware) {
  ThreadPool worker(1);
  Registrar registrar;
  const auto configuration = VideoOutputConfiguration(64, 36, hardware);
  registrar.fail_next = true;
  bool failed = false;
  try {
    VideoOutput output(1, configuration, &registrar, &worker);
  } catch (const std::runtime_error&) {
    failed = true;
  }
  Check(failed && live_contexts == 0 && registrar.Count() == 0,
        "initial registration failure leaked resources");
  auto output =
      std::make_unique<VideoOutput>(1, configuration, &registrar, &worker);
  int64_t current_id = -1;
  output->SetTextureUpdateCallback(
      [&](auto id, auto, auto) { current_id = id; });
  const auto first_id = current_id;
  registrar.fail_next = true;
  output->SetSize(80, 60);
  worker.Post([] {}).get();
  Check(current_id == first_id && registrar.Count() == 1,
        "failed replacement published or retired a texture");
  // Return to the original size after a failed registration. Rendering must
  // still target the original surface, not an unpublished replacement.
  output->SetSize(64, 36);
  worker.Post([] {}).get();
  registrar.Read(first_id, 64, 36);
  Check(wrong_surface == 0, "render target differs from registered frame");
  const auto marks = registrar.marks.load();
  fail_render = true;
  output->SetSize(64, 36);
  worker.Post([] {}).get();
  Check(registrar.marks == marks, "failed render published a frame");
  fail_render = false;
  output->SetSize(80, 60);
  worker.Post([] {}).get();
  Check(current_id != first_id, "retry after registration failure failed");
  registrar.Read(current_id, 80, 60);
  registrar.defer = true;
  auto disposal = std::async(std::launch::async, [&] { output.reset(); });
  registrar.WaitForRetirement();
  Check(disposal.wait_for(20ms) == std::future_status::timeout,
        "normal dispose must await unregister completion");
  registrar.Read(current_id, 80, 60);
  registrar.CompleteRetirements();
  disposal.get();
  Check(live_contexts == 0 && registrar.Count() == 0,
        "dispose leaked resources");
}

int main() {
  try {
    {
      ThreadPool worker(1);
      Registrar registrar;
      fail_gl = true;
      {
        VideoOutput output(1, {64, 36, true}, &registrar, &worker);
      }
      Check(gl_attempts == 1 && sw_attempts == 1 && live_contexts == 0,
            "hardware failure must cleanly fall back to software");
      fail_sw = true;
      bool failed = false;
      try {
        VideoOutput output(1, {64, 36, true}, &registrar, &worker);
      } catch (const std::runtime_error&) {
        failed = true;
      }
      Check(failed && live_contexts == 0 && registrar.Count() == 0,
            "failure of both renderers must propagate without registration");
      fail_gl = fail_sw = false;
    }
    TestFailures(false);
    TestFailures(true);
    {
      Registrar registrar;
      auto manager = std::make_unique<VideoOutputManager>(&registrar);
      std::promise<void> created, disposed;
      manager->Create(
          1, {64, 36, false}, [](auto, auto, auto) {},
          [&](auto error) {
            Check(error.empty(), "manager create");
            created.set_value();
          });
      created.get_future().get();
      registrar.defer = true;
      manager->Dispose(1, [&] { disposed.set_value(); });
      registrar.WaitForRetirement();
      // Simulate the platform thread destroying the manager while completion
      // requires that thread to return to the engine. CTest bounds deadlocks.
      manager.reset();
      disposed.get_future().get();
      registrar.Read(0, 64, 36);
      registrar.CompleteRetirements();
      Check(live_contexts == 0 && registrar.Count() == 0,
            "shutdown leaked resources");
    }
    std::cout << "PASS: fallback, failed create/register/render, resize retry, "
                 "delayed unregister and shutdown\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
  }
}
