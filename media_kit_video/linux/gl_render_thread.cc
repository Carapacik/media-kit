// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2026 Predidit.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "include/media_kit_video/gl_render_thread.h"

#include <utility>

GLRenderThread::GLRenderThread() : stop_(false) {
  thread_ = std::thread([this]() { Run(); });

  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this]() { return thread_id_ != std::thread::id(); });
}

GLRenderThread::~GLRenderThread() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_one();

  if (thread_.joinable()) {
    thread_.join();
  }
}

bool GLRenderThread::Post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_) {
      return false;
    }
    tasks_.push(std::move(task));
  }
  cv_.notify_one();
  return true;
}

void GLRenderThread::PostAndWait(std::function<void()> task) {
  if (IsCurrentThread()) {
    task();
    return;
  }

  std::mutex wait_mutex;
  std::condition_variable wait_cv;
  bool done = false;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_) {
      g_printerr(
          "media_kit: GLRenderThread: PostAndWait ignored because thread "
          "is stopped.\n");
      return;
    }

    tasks_.push([&]() {
      task();
      {
        std::lock_guard<std::mutex> wait_lock(wait_mutex);
        done = true;
      }
      wait_cv.notify_one();
    });
  }

  cv_.notify_one();

  std::unique_lock<std::mutex> lock(wait_mutex);
  wait_cv.wait(lock, [&]() { return done; });
}

bool GLRenderThread::IsCurrentThread() const {
  return std::this_thread::get_id() == thread_id_;
}

void GLRenderThread::Run() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    thread_id_ = std::this_thread::get_id();
  }
  cv_.notify_one();

  while (true) {
    std::function<void()> task;

    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });

      if (stop_ && tasks_.empty()) {
        break;
      }

      if (!tasks_.empty()) {
        task = std::move(tasks_.front());
        tasks_.pop();
      }
    }

    if (task) {
      task();
    }
  }
}
