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

#include <folly/Function.h>
#include <glog/logging.h>

/**
 * Wrappers for different versions of boost::context library
 * API reference for different versions
 * Boost 1.61:
 * https://github.com/boostorg/context/blob/boost-1.61.0/include/boost/context/detail/fcontext.hpp
 *
 * On Windows ARM64, boost::context assembly stubs (jump_fcontext / make_fcontext)
 * are not reliably available, so fall back to the native Windows Fibers API.
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
      : func_(std::move(func)), fiber_(nullptr), mainFiber_(nullptr) {
    fiber_ = CreateFiber(stackSize, &FiberImpl::fiberFunc, this);
    CHECK(fiber_ != nullptr)
        << "CreateFiber failed with error: " << GetLastError();
  }

  ~FiberImpl() {
    if (fiber_) {
      DeleteFiber(fiber_);
      fiber_ = nullptr;
    }
  }

  // Non-copyable
  FiberImpl(const FiberImpl&) = delete;
  FiberImpl& operator=(const FiberImpl&) = delete;

  // Movable — folly::fibers::Fiber requires move semantics on FiberImpl
  FiberImpl(FiberImpl&& other) noexcept
      : func_(std::move(other.func_)),
        fiber_(other.fiber_),
        mainFiber_(other.mainFiber_) {
    other.fiber_ = nullptr;
    other.mainFiber_ = nullptr;
  }

  FiberImpl& operator=(FiberImpl&& other) noexcept {
    if (this != &other) {
      // Clean up existing fiber before taking ownership
      if (fiber_) {
        DeleteFiber(fiber_);
      }
      func_ = std::move(other.func_);
      fiber_ = other.fiber_;
      mainFiber_ = other.mainFiber_;
      other.fiber_ = nullptr;
      other.mainFiber_ = nullptr;
    }
    return *this;
  }

  void activate() {
    mainFiber_ = GetCurrentFiber();
    if (mainFiber_ == nullptr || mainFiber_ == INVALID_HANDLE_VALUE) {
      mainFiber_ = ConvertThreadToFiber(nullptr);
      CHECK(mainFiber_ != nullptr)
          << "ConvertThreadToFiber failed: " << GetLastError();
    }
    SwitchToFiber(fiber_);
  }

  void deactivate() {
    CHECK(mainFiber_ != nullptr) << "deactivate() called before activate()";
    SwitchToFiber(mainFiber_);
  }

  // Stack pointer introspection not supported on Windows Fibers
  void* getStackPointer() const {
    return nullptr;
  }

 private:
  static VOID CALLBACK fiberFunc(LPVOID param) {
    auto* self = static_cast<FiberImpl*>(param);
    CHECK(self != nullptr);
    self->func_();
    // func_() should never return in normal folly::fibers usage;
    // deactivate defensively if it does.
    self->deactivate();
  }

  folly::Function<void()> func_;
  LPVOID fiber_;
  LPVOID mainFiber_;
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
      folly::Function<void()> func,
      unsigned char* stackLimit,
      size_t stackSize)
      : func_(std::move(func)) {
    auto stackBase = stackLimit + stackSize;
    stackBase_ = stackBase;
    fiberContext_ = boost::context::detail::make_fcontext(
        stackBase, stackSize, &fiberFunc);
  }

  void activate() {
    auto transfer =
        boost::context::detail::jump_fcontext(fiberContext_, this);
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
