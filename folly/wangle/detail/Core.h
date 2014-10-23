/*
 * Copyright 2014 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <folly/Optional.h>
#include <folly/SmallLocks.h>

#include <folly/wangle/Try.h>
#include <folly/wangle/Promise.h>
#include <folly/wangle/Future.h>
#include <folly/wangle/Executor.h>

namespace folly { namespace wangle { namespace detail {

// As of GCC 4.8.1, the std::function in libstdc++ optimizes only for pointers
// to functions, using a helper avoids a call to malloc.
template<typename T>
void empty_callback(Try<T>&&) { }

/** The shared state object for Future and Promise. */
template<typename T>
class Core {
 public:
  // This must be heap-constructed. There's probably a way to enforce that in
  // code but since this is just internal detail code and I don't know how
  // off-hand, I'm punting.
  Core() = default;
  ~Core() {
    assert(calledBack_);
    assert(detached_ == 2);
  }

  // not copyable
  Core(Core const&) = delete;
  Core& operator=(Core const&) = delete;

  // not movable (see comment in the implementation of Future::then)
  Core(Core&&) noexcept = delete;
  Core& operator=(Core&&) = delete;

  Try<T>& getTry() {
    if (ready()) {
      return *result_;
    } else {
      throw FutureNotReady();
    }
  }

  template <typename F>
  void setCallback(F func) {
    {
      std::lock_guard<decltype(mutex_)> lock(mutex_);

      if (callback_) {
        throw std::logic_error("setCallback called twice");
      }

      callback_ = std::move(func);
    }

    maybeCallback();
  }

  void setResult(Try<T>&& t) {
    {
      std::lock_guard<decltype(mutex_)> lock(mutex_);

      if (ready()) {
        throw std::logic_error("setResult called twice");
      }

      result_ = std::move(t);
      assert(ready());
    }

    maybeCallback();
  }

  bool ready() const {
    return result_.hasValue();
  }

  // Called by a destructing Future
  void detachFuture() {
    if (!callback_) {
      setCallback(empty_callback<T>);
    }
    activate();
    detachOne();
  }

  // Called by a destructing Promise
  void detachPromise() {
    if (!ready()) {
      setResult(Try<T>(std::make_exception_ptr(BrokenPromise())));
    }
    detachOne();
  }

  void deactivate() {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    active_ = false;
  }

  void activate() {
    {
      std::lock_guard<decltype(mutex_)> lock(mutex_);
      active_ = true;
    }
    maybeCallback();
  }

  bool isActive() { return active_; }

  void setExecutor(Executor* x) {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    executor_ = x;
  }

 private:
  void maybeCallback() {
    std::unique_lock<decltype(mutex_)> lock(mutex_);
    if (!calledBack_ &&
        result_ && callback_ && isActive()) {
      // TODO(5306911) we should probably try/catch here
      if (executor_) {
        MoveWrapper<folly::Optional<Try<T>>> val(std::move(result_));
        MoveWrapper<std::function<void(Try<T>&&)>> cb(std::move(callback_));
        executor_->add([cb, val]() mutable { (*cb)(std::move(**val)); });
        calledBack_ = true;
      } else {
        calledBack_ = true;
        lock.unlock();
        callback_(std::move(*result_));
      }
    }
  }

  void detachOne() {
    bool shouldDelete;
    {
      std::lock_guard<decltype(mutex_)> lock(mutex_);
      detached_++;
      assert(detached_ == 1 || detached_ == 2);
      shouldDelete = (detached_ == 2);
    }

    if (shouldDelete) {
      // we should have already executed the callback with the value
      assert(calledBack_);
      delete this;
    }
  }

  folly::Optional<Try<T>> result_;
  std::function<void(Try<T>&&)> callback_;
  bool calledBack_ = false;
  unsigned char detached_ = 0;
  bool active_ = true;
  Executor* executor_ = nullptr;

  // this lock isn't meant to protect all accesses to members, only the ones
  // that need to be threadsafe: the act of setting result_ and callback_, and
  // seeing if they are set and whether we should then continue.
  folly::MicroSpinLock mutex_ {0};
};

template <typename... Ts>
struct VariadicContext {
  VariadicContext() : total(0), count(0) {}
  Promise<std::tuple<Try<Ts>... > > p;
  std::tuple<Try<Ts>... > results;
  size_t total;
  std::atomic<size_t> count;
  typedef Future<std::tuple<Try<Ts>...>> type;
};

template <typename... Ts, typename THead, typename... Fs>
typename std::enable_if<sizeof...(Fs) == 0, void>::type
whenAllVariadicHelper(VariadicContext<Ts...> *ctx, THead&& head, Fs&&... tail) {
  head.setCallback_([ctx](Try<typename THead::value_type>&& t) {
    std::get<sizeof...(Ts) - sizeof...(Fs) - 1>(ctx->results) = std::move(t);
    if (++ctx->count == ctx->total) {
      ctx->p.setValue(std::move(ctx->results));
      delete ctx;
    }
  });
}

template <typename... Ts, typename THead, typename... Fs>
typename std::enable_if<sizeof...(Fs) != 0, void>::type
whenAllVariadicHelper(VariadicContext<Ts...> *ctx, THead&& head, Fs&&... tail) {
  head.setCallback_([ctx](Try<typename THead::value_type>&& t) {
    std::get<sizeof...(Ts) - sizeof...(Fs) - 1>(ctx->results) = std::move(t);
    if (++ctx->count == ctx->total) {
      ctx->p.setValue(std::move(ctx->results));
      delete ctx;
    }
  });
  // template tail-recursion
  whenAllVariadicHelper(ctx, std::forward<Fs>(tail)...);
}

template <typename T>
struct WhenAllContext {
  explicit WhenAllContext() : count(0), total(0) {}
  Promise<std::vector<Try<T> > > p;
  std::vector<Try<T> > results;
  std::atomic<size_t> count;
  size_t total;
};

template <typename T>
struct WhenAnyContext {
  explicit WhenAnyContext(size_t n) : done(false), ref_count(n) {};
  Promise<std::pair<size_t, Try<T>>> p;
  std::atomic<bool> done;
  std::atomic<size_t> ref_count;
  void decref() {
    if (--ref_count == 0) {
      delete this;
    }
  }
};

template <typename T>
struct WhenAllLaterContext {
  explicit WhenAllLaterContext() : count(0), total(0) {}
  std::function<void(std::vector<Try<T>>&&)> fn;
  std::vector<Try<T> > results;
  std::atomic<size_t> count;
  size_t total;
};

}}} // namespace