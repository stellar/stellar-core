// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
//
// StackThread.h -- a std::thread work-alike with a settable stack size.
//
// Platforms: Linux (glibc/musl), macOS, Windows (MSVC / clang-cl / MinGW-w64).
// Language:  C++17.
//
// Differences from std::thread, all deliberate:
//   * ctor takes a stack size in bytes as its first argument (0 = platform
//   default)
//   * optional thread name as a second argument, applied by the new thread
//   itself
//     (macOS only permits naming the calling thread, so this is the only
//     portable point)
//   * native_handle() is always available, not conditionally-supported
//   * id is a distinct type from native_handle_type (they differ on Windows)
//
// Everything else -- move-only, terminate-on-joinable-destruction, INVOKE-style
// argument decay-copying, terminate on escaping exception -- matches
// std::thread.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace stellar
{

namespace detail
{

// Type-erased callable. Virtual dispatch rather than a function template keeps
// the trampoline a single non-template function, which avoids instantiating a
// C-callback-shaped function per callable type.
struct PayloadBase
{
    virtual ~PayloadBase() = default;
    virtual void run() = 0;
    std::string mName;
};

template <class Fn> struct Payload final : PayloadBase
{
    Fn mFn;
    explicit Payload(Fn&& fn) : mFn(std::move(fn))
    {
    }
    void
    run() override
    {
        mFn();
    }
};

void setCurrentThreadName(std::string const& name);
#if !defined(_WIN32)
std::size_t roundStackSize(std::size_t bytes);
bool isNullThread(::pthread_t const& t) noexcept;
#endif

#if defined(_WIN32)
unsigned __stdcall trampoline(void* raw);
#else
void* trampoline(void* raw);
#endif

} // namespace detail

class StackThread
{
  public:
#if defined(_WIN32)
    using native_handle_type = HANDLE;
    using native_id_type = DWORD;
#else
    using native_handle_type = ::pthread_t;
    using native_id_type = ::pthread_t;
#endif

    // Opaque, comparable, hashable, streamable thread identity -- the analogue
    // of std::thread::id.
    class id
    {
      public:
        id() noexcept;

        explicit id(native_id_type native) noexcept;

        friend bool operator==(id const& a, id const& b) noexcept;

        friend bool operator!=(id const& a, id const& b) noexcept;

        // Ordering exists so `id` can key a std::map. POSIX gives no ordering
        // over pthread_t, so this compares the object representation. That is
        // a strict weak ordering consistent with == on every implementation
        // where pthread_t is a scalar (all of glibc, musl, macOS).
        friend bool operator<(id const& a, id const& b) noexcept;

        friend bool operator>(id const& a, id const& b) noexcept;
        friend bool operator<=(id const& a, id const& b) noexcept;
        friend bool operator>=(id const& a, id const& b) noexcept;

        template <class Char, class Traits>
        friend std::basic_ostream<Char, Traits>&
        operator<<(std::basic_ostream<Char, Traits>& os, id const& v)
        {
            if (!v.mValid)
            {
                return os << "thread::id(of a non-executing thread)";
            }
            std::uintptr_t scalar = 0;
            std::memcpy(&scalar, &v.mNative,
                        std::min(sizeof(scalar), sizeof(native_id_type)));
            return os << scalar;
        }

        std::size_t hash() const noexcept;

      private:
        native_id_type mNative;
        bool mValid;
    };

    StackThread() noexcept = default;

    // Primary constructor. stackBytes == 0 selects the platform default; any
    // other value is rounded up to satisfy platform minimums.
    template <class F, class... Args,
              class = std::enable_if_t<
                  std::is_invocable_v<std::decay_t<F>, std::decay_t<Args>...>>>
    StackThread(std::size_t stackBytes, F&& f, Args&&... args)
        : StackThread(stackBytes, std::string{}, std::forward<F>(f),
                      std::forward<Args>(args)...)
    {
    }

    // Same, but the new thread names itself before running the callable.
    template <class F, class... Args,
              class = std::enable_if_t<
                  std::is_invocable_v<std::decay_t<F>, std::decay_t<Args>...>>>
    StackThread(std::size_t stackBytes, std::string name, F&& f, Args&&... args)
    {
        // Decay-copy everything up front, exactly as std::thread does, so the
        // new thread never touches the caller's storage.
        auto bound =
            [tup = std::make_tuple(
                 std::decay_t<F>(std::forward<F>(f)),
                 std::decay_t<Args>(std::forward<Args>(args))...)]() mutable {
                std::apply(
                    [](auto&& fn, auto&&... rest) {
                        std::invoke(std::forward<decltype(fn)>(fn),
                                    std::forward<decltype(rest)>(rest)...);
                    },
                    std::move(tup));
            };

        auto payload = std::make_unique<detail::Payload<decltype(bound)>>(
            std::move(bound));
        payload->mName = std::move(name);

        start(stackBytes, std::move(payload));
    }

    StackThread(StackThread const&) = delete;
    StackThread& operator=(StackThread const&) = delete;

    StackThread(StackThread&& other) noexcept;

    StackThread& operator=(StackThread&& other) noexcept;

    ~StackThread();

    bool joinable() const noexcept;

    void join();

    void detach();

    id get_id() const noexcept;

    // Valid only while joinable(). On POSIX this is the pthread_t, suitable for
    // pthread_setaffinity_np, pthread_setschedparam, pthread_getattr_np, etc.
    //
    // Caveat: joinable() only means "not yet joined or detached", not "still
    // running". Once the thread function returns, the pthread_t stays valid as
    // a join target but the underlying kernel task is gone, so scheduling and
    // affinity calls will fail with ESRCH. If you intend to pin or reprioritise
    // a thread, do it promptly after construction, or have the thread do it to
    // itself. (Windows HANDLEs do not have this problem -- they stay queryable
    // after exit.)
    native_handle_type native_handle() const noexcept;

    void swap(StackThread& other) noexcept;

    static unsigned int hardware_concurrency() noexcept;

    // Convenience: name the calling thread. Note macOS can only name itself,
    // so there is intentionally no name-another-thread entry point.
    static void setCurrentName(std::string const& name);

  private:
    void start(std::size_t stackBytes,
               std::unique_ptr<detail::PayloadBase> payload);

#if defined(_WIN32)
    HANDLE mHandle = nullptr;
#else
    ::pthread_t mThread{};
#endif
};

void swap(StackThread& a, StackThread& b) noexcept;

} // namespace stellar

namespace std
{
template <> struct hash<::stellar::StackThread::id>
{
    std::size_t operator()(::stellar::StackThread::id const& v) const noexcept;
};
} // namespace std
