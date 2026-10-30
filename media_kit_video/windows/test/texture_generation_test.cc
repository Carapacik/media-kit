#include "../angle_surface_manager.h"
#include "../thread_pool.h"

#include <atomic>
#include <iostream>
#include <stdexcept>

using Generation = ANGLESurfaceManager::TextureGeneration;

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void CheckColor(const std::shared_ptr<Generation>& frame, bool red) {
  frame->Read();
  std::lock_guard<std::mutex> lock(*frame->mutex);
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  frame->destination->GetDevice(&device);
  D3D11_TEXTURE2D_DESC description{};
  frame->destination->GetDesc(&description);
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.MiscFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  Check(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &staging)),
        "staging texture");
  frame->context->CopyResource(staging.Get(), frame->destination.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  Check(SUCCEEDED(
            frame->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
        "readback");
  const auto* pixel = static_cast<const uint8_t*>(mapped.pData);
  const bool matches =
      red ? pixel[2] == 255 && pixel[1] == 0 : pixel[1] == 255 && pixel[2] == 0;
  frame->context->Unmap(staging.Get(), 0);
  Check(matches, "retained generation pixels changed");
}

int main() {
  try {
    std::shared_ptr<Generation> old, current;
    {
      ANGLESurfaceManager manager(64, 36);
      bool failed = false;
      try {
        ANGLESurfaceManager invalid(-1, 0);
      } catch (const std::runtime_error&) {
        failed = true;
      }
      Check(failed,
            "invalid construction must fail without affecting live output");
      manager.Draw([] {
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
      });
      old = manager.GetTextureGeneration();
      for (int attempt = 0; attempt < 3; ++attempt) {
        failed = false;
        try {
          // D3D rejects this size after resize has begun, without allocating a
          // huge texture. The existing surface must survive the failed call.
          manager.SetSize(-1, 36);
        } catch (const std::runtime_error&) {
          failed = true;
        }
        Check(failed && manager.width() == 64 && manager.height() == 36 &&
                  manager.handle() == old->handle,
              "failed resize must restore dimensions and shared handle");
        manager.Draw([] {
          glClearColor(1, 0, 0, 1);
          glClear(GL_COLOR_BUFFER_BIT);
        });
        CheckColor(old, true);
      }
      try {
        manager.Draw([] { throw std::runtime_error("injected draw failure"); });
      } catch (const std::runtime_error&) {
      }
      Check(eglGetCurrentContext() == EGL_NO_CONTEXT,
            "failed Draw must unbind the context");
      manager.SetSize(80, 60);
      manager.Draw([] {
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
      });
      current = manager.GetTextureGeneration();
      Check(old->handle != current->handle && old->width == 64 &&
                current->width == 80,
            "resize must preserve old handle and dimensions");
      CheckColor(old, true);
      CheckColor(current, false);
      // Simulate Flutter asking for a retired registration during repeated
      // resize/Draw on the rendering worker, then during manager teardown.
      auto reader = std::async(std::launch::async, [old] {
        for (int i = 0; i < 100; ++i)
          CheckColor(old, true);
      });
      for (int i = 0; i < 100; ++i) {
        manager.SetSize(80 + i % 2, 60);
        manager.Draw([] {
          glClearColor(0, 1, 0, 1);
          glClear(GL_COLOR_BUFFER_BIT);
        });
      }
      reader.get();
    }
    CheckColor(old, true);
    CheckColor(current, false);
    old.reset();
    current.reset();

    std::vector<int> order;
    {
      ThreadPool operations(1);
      for (int i = 0; i < 100; ++i) {
        operations.Post([&order, i] { order.push_back(i); });
      }
    }
    Check(order.size() == 100, "shutdown must drain accepted operations");
    for (int i = 0; i < 100; ++i)
      Check(order[i] == i, "FIFO operation order");
    std::cout << "PASS: retained GPU generations, concurrent resize/read, "
                 "teardown, FIFO shutdown\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
  }
}
