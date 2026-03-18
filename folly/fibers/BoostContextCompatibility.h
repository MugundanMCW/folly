/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <glog/logging.h>
#include <folly/Function.h>

/**
 * Wrappers for different versions of boost::context library
 * API reference for different versions
 * Boost 1.61:
 * https://github.com/boostorg/context/blob/boost-1.61.0/include/boost/context/detail/fcontext.hpp
 *
 * On Windows ARM64, boost::context assembly stubs (jump_fcontext / make_fcontext)
 * are not currently supported, so fall back to the native Windows Fibers API.
 */

#if defined(_WIN32) && defined(_M_ARM64)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace folly {
namespace fibers {

class FiberImpl {
 public:
  FiberImpl(
      folly::Function<void()> func,
      unsigned char* /*stackLimit*/,
      size_t stackSize)
      : func_(std::move(func)) {
    fiber_ = CreateFiber(stackSize, &FiberImpl::fiberFunc, this);
    CHECK(fiber_ != nullptr)
        << "CreateFiber failed: " << GetLastError();
  }

  ~FiberImpl() {
      if (fiber_) {
          DCHECK(GetCurrentFiber() != fiber_)
              << "Destroying fiber while it is active";
          DeleteFiber(std::exchange(fiber_, nullptr));
      }
  }

  FiberImpl(const FiberImpl&) = delete;
  FiberImpl& operator=(const FiberImpl&) = delete;

  FiberImpl(FiberImpl&& other) noexcept
      : func_(std::move(other.func_)),
        fiber_(std::exchange(other.fiber_, nullptr)),
        mainFiber_(std::exchange(other.mainFiber_, nullptr)) {}

  FiberImpl& operator=(FiberImpl&& other) noexcept {
    if (this != &other) {
      if (fiber_) DeleteFiber(fiber_);
      func_ = std::move(other.func_);
      fiber_ = std::exchange(other.fiber_, nullptr);
      mainFiber_ = std::exchange(other.mainFiber_, nullptr);
    }
    return *this;
  }

  void activate() {
      mainFiber_ = GetCurrentFiber();
      // GetCurrentFiber() returns INVALID_HANDLE_VALUE (not nullptr)
      // when the thread has not been converted to a fiber yet.
      if (mainFiber_ == INVALID_HANDLE_VALUE) {
          mainFiber_ = ConvertThreadToFiber(nullptr);
          CHECK(mainFiber_ != nullptr)
              << "ConvertThreadToFiber failed: " << GetLastError();
      }
      SwitchToFiber(fiber_);
  }


  void deactivate() {
    DCHECK(mainFiber_ != nullptr) << "deactivate() called before activate()";
    SwitchToFiber(std::exchange(mainFiber_, nullptr));
  }

  void* getStackPointer() const { return nullptr; }

 private:
  static VOID CALLBACK fiberFunc(LPVOID param) {
    auto* self = static_cast<FiberImpl*>(param);
    self->func_();
    // func_() must not return in normal folly::fibers usage.
    // If it does, switch back to avoid undefined behavior — but
    // don't call deactivate() as mainFiber_ may already be cleared.
    CHECK(false) << "FiberImpl::func_() returned unexpectedly";
  }

  folly::Function<void()> func_;
  LPVOID fiber_{nullptr};
  LPVOID mainFiber_{nullptr};
};

} // namespace fibers
} // namespace folly
#else

#include <boost/context/detail/fcontext.hpp>

namespace folly {
namespace fibers {

class FiberImpl {
  using FiberContext = boost::context::detail::fcontext_t;

  using MainContext = boost::context::detail::fcontext_t;

 public:
  FiberImpl(
      folly::Function<void()> func, unsigned char* stackLimit, size_t stackSize)
      : func_(std::move(func)) {
    auto stackBase = stackLimit + stackSize;
    stackBase_ = stackBase;
    fiberContext_ =
        boost::context::detail::make_fcontext(stackBase, stackSize, &fiberFunc);
  }

  void activate() {
    auto transfer = boost::context::detail::jump_fcontext(fiberContext_, this);
    fiberContext_ = transfer.fctx;
    auto context = reinterpret_cast<intptr_t>(transfer.data);
    DCHECK_EQ(0, context);
  }

  void deactivate() {
    auto transfer =
        boost::context::detail::jump_fcontext(mainContext_, nullptr);
    mainContext_ = transfer.fctx;
    fixStackUnwinding();
    auto context = reinterpret_cast<intptr_t>(transfer.data);
    DCHECK_EQ(this, reinterpret_cast<FiberImpl*>(context));
  }

  void* getStackPointer() const {
    if (kIsArchAmd64 && kIsLinux) {
      return reinterpret_cast<void**>(fiberContext_)[6];
    }
    return nullptr;
  }

 private:
  static void fiberFunc(boost::context::detail::transfer_t transfer) {
    auto fiberImpl = reinterpret_cast<FiberImpl*>(transfer.data);
    fiberImpl->mainContext_ = transfer.fctx;
    fiberImpl->fixStackUnwinding();
    fiberImpl->func_();
  }

  void fixStackUnwinding() {
    if (kIsArchAmd64 && kIsLinux) {
      // Extract RBP and RIP from main context to stitch main context stack and
      // fiber stack.
      auto stackBase = reinterpret_cast<void**>(stackBase_);
      auto mainContext = reinterpret_cast<void**>(mainContext_);
      stackBase[-2] = mainContext[6];
      stackBase[-1] = mainContext[7];
    }
  }

  unsigned char* stackBase_;
  folly::Function<void()> func_;
  FiberContext fiberContext_;
  MainContext mainContext_;
};
} // namespace fibers
} // namespace folly
#endif

