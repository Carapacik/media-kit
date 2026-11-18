#include "../platform_task_runner.h"

#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void Pump() {
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

int main() {
  try {
    using media_kit_video::PlatformTaskRunner;
    const auto platform_thread = GetCurrentThreadId();
    PlatformTaskRunner first;
    PlatformTaskRunner second;
    std::vector<int> order;
    bool correct_thread = true;
    std::thread worker([&] {
      for (int i = 0; i < 100; ++i) {
        Check(first.Post([&, i] {
          correct_thread &= GetCurrentThreadId() == platform_thread;
          order.push_back(i);
        }),
              "failed to post from worker");
      }
    });
    worker.join();
    Check(order.empty(), "callback ran on worker thread");
    Pump();
    Check(correct_thread, "wrong callback thread");
    Check(order.size() == 100, "callbacks lost");
    for (int i = 0; i < 100; ++i)
      Check(order[i] == i, "callbacks reordered");

    int cancelled = 0, surviving = 0;
    first.Post([&] { ++cancelled; });
    second.Post([&] { ++surviving; });
    first.Close();
    Check(!first.Post([&] { ++cancelled; }), "closed runner accepts work");
    Pump();
    Check(cancelled == 0 && surviving == 1, "runner lifetime not isolated");
    second.Post([] { throw std::runtime_error("injected callback failure"); });
    second.Post([&] { ++surviving; });
    Pump();
    Check(surviving == 2, "exception blocked later callbacks");
    {
      PlatformTaskRunner recreated;
      recreated.Post([&] { ++cancelled; });
    }
    Pump();
    Check(cancelled == 0, "callback ran after destruction");
    PlatformTaskRunner recreated;
    recreated.Post([&] { ++surviving; });
    Pump();
    Check(surviving == 3, "runner recreation failed");
    std::cout << "Platform task runner checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
