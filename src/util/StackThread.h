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
//   * optional thread name as an second argument, applied by the new thread
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
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <system_error>
#include <thread>
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
// windows.h first, then:
#include <climits>
#include <process.h>
#ifndef STACK_SIZE_PARAM_IS_A_RESERVATION
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#endif
#else
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
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

inline void
setCurrentThreadName(std::string const& name)
{
    if (name.empty())
    {
        return;
    }
#if defined(_WIN32)
    // SetThreadDescription is Windows 10 1607+; resolve dynamically so the
    // binary still loads on older systems.
    using SetDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static SetDescFn const setDesc =
        reinterpret_cast<SetDescFn>(reinterpret_cast<void*>(::GetProcAddress(
            ::GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (setDesc != nullptr)
    {
        // Thread names are ASCII in practice; widen naively.
        std::wstring wide(name.begin(), name.end());
        setDesc(::GetCurrentThread(), wide.c_str());
    }
#elif defined(__APPLE__)
    // Self-only, and silently truncates.
    ::pthread_setname_np(name.c_str());
#elif defined(__linux__)
    // Hard limit of 16 bytes including the NUL; longer names fail with ERANGE.
    std::string const truncated = name.substr(0, 15);
    ::pthread_setname_np(::pthread_self(), truncated.c_str());
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    ::pthread_set_name_np(::pthread_self(), name.c_str());
#else
    (void)name;
#endif
}

#if !defined(_WIN32)
// pthread_attr_setstacksize requires a multiple of the page size and at least
// PTHREAD_STACK_MIN. On glibc >= 2.34 PTHREAD_STACK_MIN is no longer a
// compile-time constant on every architecture, so prefer sysconf.
inline std::size_t
roundStackSize(std::size_t bytes)
{
    long const pageRaw = ::sysconf(_SC_PAGESIZE);
    std::size_t const page =
        (pageRaw > 0) ? static_cast<std::size_t>(pageRaw) : 4096u;

    std::size_t minStack = page;
#if defined(_SC_THREAD_STACK_MIN)
    long const minRaw = ::sysconf(_SC_THREAD_STACK_MIN);
    if (minRaw > 0)
    {
        minStack = std::max(minStack, static_cast<std::size_t>(minRaw));
    }
#endif
#if defined(PTHREAD_STACK_MIN)
    minStack = std::max(minStack, static_cast<std::size_t>(PTHREAD_STACK_MIN));
#endif

    bytes = std::max(bytes, minStack);
    // Round up to a page multiple, guarding against overflow.
    if (bytes > (~static_cast<std::size_t>(0)) - (page - 1))
    {
        return bytes - (bytes % page);
    }
    return ((bytes + page - 1) / page) * page;
}
#endif

// Note: passing a C++-linkage function to pthread_create is formally
// unspecified, but is what every real implementation (and libstdc++/libc++
// themselves) does.
#if defined(_WIN32)
inline unsigned __stdcall trampoline(void* raw)
#else
inline void*
trampoline(void* raw)
#endif
{
    std::unique_ptr<PayloadBase> payload(static_cast<PayloadBase*>(raw));
    try
    {
        setCurrentThreadName(payload->mName);
        payload->run();
    }
    catch (...)
    {
        // std::thread's contract: an exception escaping the thread function
        // calls std::terminate. Letting it unwind into the C runtime here
        // would be undefined behaviour, so make it explicit.
        std::terminate();
    }
#if defined(_WIN32)
    return 0u;
#else
    return nullptr;
#endif
}

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
        id() noexcept : mNative{}, mValid(false)
        {
        }

        explicit id(native_id_type native) noexcept
            : mNative(native), mValid(true)
        {
        }

        friend bool
        operator==(id const& a, id const& b) noexcept
        {
            if (a.mValid != b.mValid)
            {
                return false;
            }
            if (!a.mValid)
            {
                return true;
            }
#if defined(_WIN32)
            return a.mNative == b.mNative;
#else
            return ::pthread_equal(a.mNative, b.mNative) != 0;
#endif
        }

        friend bool
        operator!=(id const& a, id const& b) noexcept
        {
            return !(a == b);
        }

        // Ordering exists so `id` can key a std::map. POSIX gives no ordering
        // over pthread_t, so this compares the object representation. That is
        // a strict weak ordering consistent with == on every implementation
        // where pthread_t is a scalar (all of glibc, musl, macOS).
        friend bool
        operator<(id const& a, id const& b) noexcept
        {
            if (a.mValid != b.mValid)
            {
                return !a.mValid;
            }
            if (!a.mValid)
            {
                return false;
            }
            return std::memcmp(&a.mNative, &b.mNative, sizeof(native_id_type)) <
                   0;
        }

        friend bool
        operator>(id const& a, id const& b) noexcept
        {
            return b < a;
        }
        friend bool
        operator<=(id const& a, id const& b) noexcept
        {
            return !(b < a);
        }
        friend bool
        operator>=(id const& a, id const& b) noexcept
        {
            return !(a < b);
        }

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

        std::size_t
        hash() const noexcept
        {
            if (!mValid)
            {
                return 0;
            }
            std::uintptr_t scalar = 0;
            std::memcpy(&scalar, &mNative,
                        std::min(sizeof(scalar), sizeof(native_id_type)));
            return std::hash<std::uintptr_t>{}(scalar);
        }

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

    StackThread(StackThread&& other) noexcept
    {
        swap(other);
    }

    StackThread&
    operator=(StackThread&& other) noexcept
    {
        if (this != &other)
        {
            if (joinable())
            {
                // Matching std::thread: assigning over a joinable thread is
                // a programming error, not a silent detach.
                std::terminate();
            }
            swap(other);
        }
        return *this;
    }

    ~StackThread()
    {
        if (joinable())
        {
            std::terminate();
        }
    }

    bool
    joinable() const noexcept
    {
#if defined(_WIN32)
        return mHandle != nullptr;
#else
        return mJoinable;
#endif
    }

    void
    join()
    {
        if (!joinable())
        {
            throw std::system_error(
                std::make_error_code(std::errc::invalid_argument),
                "StackThread::join on a non-joinable thread");
        }
#if defined(_WIN32)
        DWORD const rc = ::WaitForSingleObject(mHandle, INFINITE);
        if (rc != WAIT_OBJECT_0)
        {
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "WaitForSingleObject");
        }
        ::CloseHandle(mHandle);
        mHandle = nullptr;
#else
        int const rc = ::pthread_join(mThread, nullptr);
        if (rc != 0)
        {
            throw std::system_error(rc, std::system_category(), "pthread_join");
        }
        mJoinable = false;
#endif
        mId = id{};
    }

    void
    detach()
    {
        if (!joinable())
        {
            throw std::system_error(
                std::make_error_code(std::errc::invalid_argument),
                "StackThread::detach on a non-joinable thread");
        }
#if defined(_WIN32)
        ::CloseHandle(mHandle);
        mHandle = nullptr;
#else
        int const rc = ::pthread_detach(mThread);
        if (rc != 0)
        {
            throw std::system_error(rc, std::system_category(),
                                    "pthread_detach");
        }
        mJoinable = false;
#endif
        mId = id{};
    }

    id
    get_id() const noexcept
    {
        return mId;
    }

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
    native_handle_type
    native_handle() const noexcept
    {
#if defined(_WIN32)
        return mHandle;
#else
        return mThread;
#endif
    }

    void
    swap(StackThread& other) noexcept
    {
#if defined(_WIN32)
        std::swap(mHandle, other.mHandle);
#else
        std::swap(mThread, other.mThread);
        std::swap(mJoinable, other.mJoinable);
#endif
        std::swap(mId, other.mId);
    }

    static unsigned int
    hardware_concurrency() noexcept
    {
        return std::thread::hardware_concurrency();
    }

    // Convenience: name the calling thread. Note macOS can only name itself,
    // so there is intentionally no name-another-thread entry point.
    static void
    setCurrentName(std::string const& name)
    {
        detail::setCurrentThreadName(name);
    }

  private:
    void
    start(std::size_t stackBytes, std::unique_ptr<detail::PayloadBase> payload)
    {
#if defined(_WIN32)
        if (stackBytes > static_cast<std::size_t>(UINT_MAX))
        {
            throw std::system_error(
                std::make_error_code(std::errc::invalid_argument),
                "requested stack size exceeds the Win32 unsigned limit");
        }
        // Without STACK_SIZE_PARAM_IS_A_RESERVATION the size argument is the
        // initial *commit*, and the reserve still comes from the PE header.
        unsigned threadId = 0;
        uintptr_t const h = ::_beginthreadex(
            nullptr, static_cast<unsigned>(stackBytes), &detail::trampoline,
            payload.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, &threadId);
        if (h == 0)
        {
            throw std::system_error(errno, std::generic_category(),
                                    "_beginthreadex");
        }
        payload.release(); // ownership transferred to the new thread
        mHandle = reinterpret_cast<HANDLE>(h);
        mId = id{static_cast<DWORD>(threadId)};
#else
        ::pthread_attr_t attr;
        int rc = ::pthread_attr_init(&attr);
        if (rc != 0)
        {
            throw std::system_error(rc, std::system_category(),
                                    "pthread_attr_init");
        }
        struct AttrGuard
        {
            ::pthread_attr_t* a;
            ~AttrGuard()
            {
                ::pthread_attr_destroy(a);
            }
        } guard{&attr};

        if (stackBytes != 0)
        {
            rc = ::pthread_attr_setstacksize(
                &attr, detail::roundStackSize(stackBytes));
            if (rc != 0)
            {
                throw std::system_error(rc, std::system_category(),
                                        "pthread_attr_setstacksize");
            }
        }

        ::pthread_t tid{};
        rc = ::pthread_create(&tid, &attr, &detail::trampoline, payload.get());
        if (rc != 0)
        {
            throw std::system_error(rc, std::system_category(),
                                    "pthread_create");
        }
        payload.release(); // ownership transferred to the new thread
        mThread = tid;
        mJoinable = true;
        mId = id{tid};
#endif
    }

#if defined(_WIN32)
    HANDLE mHandle = nullptr;
#else
    ::pthread_t mThread{};
    bool mJoinable = false;
#endif
    id mId{};
};

inline void
swap(StackThread& a, StackThread& b) noexcept
{
    a.swap(b);
}

} // namespace stellar

namespace std
{
template <> struct hash<::stellar::StackThread::id>
{
    std::size_t
    operator()(::stellar::StackThread::id const& v) const noexcept
    {
        return v.hash();
    }
};
} // namespace std