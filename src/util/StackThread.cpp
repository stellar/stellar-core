// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/StackThread.h"

#include <exception>
#include <system_error>
#include <thread>

#if defined(_WIN32)
// windows.h first, then:
#include <climits>
#include <process.h>
#ifndef STACK_SIZE_PARAM_IS_A_RESERVATION
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#endif
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace stellar
{

namespace detail
{

void
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
    // Hard limit of 64 bytes including the NUL; longer names fail with ERANGE.
    std::string const truncated = name.substr(0, 63);
    ::pthread_setname_np(truncated.c_str());
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
std::size_t
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

bool
isNullThread(::pthread_t const& t) noexcept
{
    ::pthread_t nullThread{};
    return std::memcmp(&t, &nullThread, sizeof(t)) == 0;
}
#endif

// Note: passing a C++-linkage function to pthread_create is formally
// unspecified, but is what every real implementation (and libstdc++/libc++
// themselves) does.
#if defined(_WIN32)
unsigned __stdcall trampoline(void* raw)
#else
void*
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

StackThread::id::id() noexcept : mNative{}, mValid(false)
{
}

StackThread::id::id(native_id_type native) noexcept
    : mNative(native), mValid(true)
{
}

bool
operator==(StackThread::id const& a, StackThread::id const& b) noexcept
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

bool
operator!=(StackThread::id const& a, StackThread::id const& b) noexcept
{
    return !(a == b);
}

bool
operator<(StackThread::id const& a, StackThread::id const& b) noexcept
{
    if (a.mValid != b.mValid)
    {
        return !a.mValid;
    }
    if (!a.mValid)
    {
        return false;
    }
    return std::memcmp(&a.mNative, &b.mNative,
                       sizeof(StackThread::native_id_type)) < 0;
}

bool
operator>(StackThread::id const& a, StackThread::id const& b) noexcept
{
    return b < a;
}

bool
operator<=(StackThread::id const& a, StackThread::id const& b) noexcept
{
    return !(b < a);
}

bool
operator>=(StackThread::id const& a, StackThread::id const& b) noexcept
{
    return !(a < b);
}

std::size_t
StackThread::id::hash() const noexcept
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

StackThread::StackThread(StackThread&& other) noexcept
{
    swap(other);
}

StackThread&
StackThread::operator=(StackThread&& other) noexcept
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

StackThread::~StackThread()
{
    if (joinable())
    {
        std::terminate();
    }
}

bool
StackThread::joinable() const noexcept
{
#if defined(_WIN32)
    return mHandle != nullptr;
#else
    return !detail::isNullThread(mThread);
#endif
}

void
StackThread::join()
{
    if (!joinable())
    {
        throw std::system_error(
            std::make_error_code(std::errc::invalid_argument),
            "StackThread::join on a non-joinable thread");
    }
#if defined(_WIN32)
    if (::GetThreadId(mHandle) == ::GetCurrentThreadId())
    {
        throw std::system_error(
            std::make_error_code(std::errc::resource_deadlock_would_occur),
            "StackThread::join on itself");
    }
    DWORD const rc = ::WaitForSingleObject(mHandle, INFINITE);
    if (rc != WAIT_OBJECT_0)
    {
        throw std::system_error(static_cast<int>(::GetLastError()),
                                std::system_category(), "WaitForSingleObject");
    }
    ::CloseHandle(mHandle);
    mHandle = nullptr;
#else
    int const rc = ::pthread_join(mThread, nullptr);
    if (rc != 0)
    {
        throw std::system_error(rc, std::system_category(), "pthread_join");
    }
    mThread = native_handle_type{};
#endif
}

void
StackThread::detach()
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
        throw std::system_error(rc, std::system_category(), "pthread_detach");
    }
    mThread = native_handle_type{};
#endif
}

StackThread::id
StackThread::get_id() const noexcept
{
    if (!joinable())
    {
        return id{};
    }
#if defined(_WIN32)
    return id{::GetThreadId(mHandle)};
#else
    return id{mThread};
#endif
}

StackThread::native_handle_type
StackThread::native_handle() const noexcept
{
#if defined(_WIN32)
    return mHandle;
#else
    return mThread;
#endif
}

void
StackThread::swap(StackThread& other) noexcept
{
#if defined(_WIN32)
    std::swap(mHandle, other.mHandle);
#else
    std::swap(mThread, other.mThread);
#endif
}

unsigned int
StackThread::hardware_concurrency() noexcept
{
    return std::thread::hardware_concurrency();
}

void
StackThread::setCurrentName(std::string const& name)
{
    detail::setCurrentThreadName(name);
}

void
StackThread::start(std::size_t stackBytes,
                   std::unique_ptr<detail::PayloadBase> payload)
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
    // _beginthreadex may start executing the trampoline before it returns, so
    // create the thread suspended until mHandle is published. This preserves
    // std::thread-like constructor synchronization for callables that capture
    // the StackThread object under construction.
    uintptr_t const h = ::_beginthreadex(
        nullptr, static_cast<unsigned>(stackBytes), &detail::trampoline,
        payload.get(), STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED,
        nullptr);
    if (h == 0)
    {
        throw std::system_error(errno, std::generic_category(),
                                "_beginthreadex");
    }
    mHandle = reinterpret_cast<HANDLE>(h);
    [[maybe_unused]] auto* transferredPayload = payload.release();
    if (::ResumeThread(mHandle) == static_cast<DWORD>(-1))
    {
        DWORD const ec = ::GetLastError();
        ::CloseHandle(mHandle);
        mHandle = nullptr;
        throw std::system_error(static_cast<int>(ec), std::system_category(),
                                "ResumeThread");
    }
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
        rc = ::pthread_attr_setstacksize(&attr,
                                         detail::roundStackSize(stackBytes));
        if (rc != 0)
        {
            throw std::system_error(rc, std::system_category(),
                                    "pthread_attr_setstacksize");
        }
    }

    rc = ::pthread_create(&mThread, &attr, &detail::trampoline, payload.get());
    if (rc != 0)
    {
        throw std::system_error(rc, std::system_category(), "pthread_create");
    }
    [[maybe_unused]] auto* transferredPayload = payload.release();
#endif
}

void
swap(StackThread& a, StackThread& b) noexcept
{
    a.swap(b);
}

} // namespace stellar

namespace std
{

std::size_t
hash<::stellar::StackThread::id>::operator()(
    ::stellar::StackThread::id const& v) const noexcept
{
    return v.hash();
}

} // namespace std