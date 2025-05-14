#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Thread safety annotation macros for use with Clang's thread safety analysis
// feature. These annotations allow the compiler to warn about potential thread
// safety issues at compile time.
//
// Documentation for the annotations is available at:
// https://clang.llvm.org/docs/ThreadSafetyAnalysis.html

#ifndef THREAD_ANNOTATIONS_H_
#define THREAD_ANNOTATIONS_H_

#include <mutex>
#include <shared_mutex>

#if defined(__clang__) && (!defined(SWIG))
#define THREAD_ANNOTATION_ATTRIBUTE__(x) __attribute__((x))
#else
#define THREAD_ANNOTATION_ATTRIBUTE__(x) // no-op
#endif

// Declares that a class member (or all members of a class) is protected by
// the given capability.
#define GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))

// Declares that a class member (or all members of a class) is protected by
// the given capability, but only when the class instance is accessed through
// a pointer.
#define PT_GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))

// Declares that a capability is a mutex, or a reader-writer lock.
#define CAPABILITY(x) THREAD_ANNOTATION_ATTRIBUTE__(capability(x))

// Declares that a capability must be acquired before this one.
#define ACQUIRED_BEFORE(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(acquired_before(__VA_ARGS__))

// Declares that a capability must be acquired after this one.
#define ACQUIRED_AFTER(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(acquired_after(__VA_ARGS__))

// Declares that a function requires certain capabilities to be held by the
// caller.
#define REQUIRES(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(requires_capability(__VA_ARGS__))

// Declares that a function requires shared access to certain capabilities to
// be held by the caller.
#define REQUIRES_SHARED(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(requires_shared_capability(__VA_ARGS__))

// Declares that a function acquires a capability.
#define ACQUIRE(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(acquire_capability(__VA_ARGS__))

// Declares that a function acquires shared access to a capability.
#define ACQUIRE_SHARED(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(acquire_shared_capability(__VA_ARGS__))

// Declares that a function releases a capability.
#define RELEASE(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(release_capability(__VA_ARGS__))

// Declares that a function releases shared access to a capability.
#define RELEASE_SHARED(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(release_shared_capability(__VA_ARGS__))

// Tries to acquire a capability, returning true on success.
#define TRY_ACQUIRE(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_capability(__VA_ARGS__))

// Tries to acquire shared access to a capability, returning true on success.
#define TRY_ACQUIRE_SHARED(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_shared_capability(__VA_ARGS__))

// Asserts that a capability is held when entering the function.
#define ASSERT_CAPABILITY(x) THREAD_ANNOTATION_ATTRIBUTE__(assert_capability(x))

// Asserts that shared access to a capability is held when entering the
// function.
#define ASSERT_SHARED_CAPABILITY(x) \
    THREAD_ANNOTATION_ATTRIBUTE__(assert_shared_capability(x))

// Return value represents a capability that is acquired.
#define RETURN_CAPABILITY(x) THREAD_ANNOTATION_ATTRIBUTE__(lock_returned(x))

// Skip thread safety analysis on this function.
#define NO_THREAD_SAFETY_ANALYSIS \
    THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)

// Declares that a function acquires a mutex exclusively.
#define EXCLUSIVE_LOCK_FUNCTION(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(exclusive_lock_function(__VA_ARGS__))

// Declares that a function acquires a mutex for shared (read) access.
#define SHARED_LOCK_FUNCTION(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(shared_lock_function(__VA_ARGS__))

// Declares that a function tries to acquire a mutex exclusively, returning true
// on success.
#define EXCLUSIVE_TRYLOCK_FUNCTION(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(exclusive_trylock_function(__VA_ARGS__))

// Declares that a function tries to acquire a mutex for shared access,
// returning true on success.
#define SHARED_TRYLOCK_FUNCTION(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(shared_trylock_function(__VA_ARGS__))

// Declares that a function releases a mutex.
#define UNLOCK_FUNCTION(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(unlock_function(__VA_ARGS__))

// Declares that a function requires the specified locks not to be held when the
// function is called.
#define LOCKS_EXCLUDED(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(locks_excluded(__VA_ARGS__))

// Declares that a function returns a value representing a mutex.
#define LOCK_RETURNED(x) THREAD_ANNOTATION_ATTRIBUTE__(lock_returned(x))

// Declares that a class is a lockable type (such as the Mutex class).
#define LOCKABLE THREAD_ANNOTATION_ATTRIBUTE__(lockable)

// Declares that a class is a scoped lockable type (such as the MutexLocker
// class).
#define SCOPED_LOCKABLE THREAD_ANNOTATION_ATTRIBUTE__(scoped_lockable)

// Defines an annotated interface for mutexes.
// These methods can be implemented to use any internal mutex implementation.
class LOCKABLE Mutex
{
  private:
    std::mutex mMutex;

  public:
    // Acquire/lock this mutex exclusively.  Only one thread can have exclusive
    // access at any one time.  Write operations to guarded data require an
    // exclusive lock.
    void
    Lock() EXCLUSIVE_LOCK_FUNCTION()
    {
        mMutex.lock();
    }

    // Release/unlock the mutex, regardless of whether it is exclusive or
    // shared.
    void
    Unlock() UNLOCK_FUNCTION()
    {
        mMutex.unlock();
    }
};

// MutexLocker is an RAII class that acquires a mutex in its constructor, and
// releases it in its destructor.
template <typename MutexType> class SCOPED_LOCKABLE MutexLockerT
{
  private:
    MutexType& mut;

  public:
    MutexLockerT(MutexType& mu) EXCLUSIVE_LOCK_FUNCTION(mu) : mut(mu)
    {
        mu.Lock();
    }
    ~MutexLockerT() UNLOCK_FUNCTION()
    {
        mut.Unlock();
    }
};

// Defines an annotated interface for shared mutexes (read-write locks).
// These methods can be implemented to use any internal shared_mutex
// implementation.
class LOCKABLE SharedMutex
{
  private:
    std::shared_mutex mSharedMutex;

  public:
    // Acquire/lock this mutex exclusively (for writing).
    // Only one thread can have exclusive access at any one time.
    void
    Lock() EXCLUSIVE_LOCK_FUNCTION()
    {
        mSharedMutex.lock();
    }

    // Acquire/lock this mutex for shared (read-only) access.
    // Multiple threads can acquire the mutex simultaneously for shared access.
    void
    LockShared() SHARED_LOCK_FUNCTION()
    {
        mSharedMutex.lock_shared();
    }

    // Release/unlock the mutex from exclusive mode.
    void
    Unlock() UNLOCK_FUNCTION()
    {
        mSharedMutex.unlock();
    }

    // Release/unlock the mutex from shared mode.
    void
    UnlockShared() UNLOCK_FUNCTION()
    {
        mSharedMutex.unlock_shared();
    }
};

// SharedLockShared is an RAII class that acquires a shared mutex in shared
// mode in its constructor, and releases it in its destructor.
class SCOPED_LOCKABLE SharedLockShared
{
  private:
    SharedMutex& mut;

  public:
    SharedLockShared(SharedMutex& mu) SHARED_LOCK_FUNCTION(mu) : mut(mu)
    {
        mu.LockShared();
    }
    ~SharedLockShared() UNLOCK_FUNCTION()
    {
        mut.UnlockShared();
    }
};

// Defines an annotated interface for recursive mutexes.
// These methods can be implemented to use any internal recursive_mutex
// implementation.
class LOCKABLE RecursiveMutex
{
  private:
    std::recursive_mutex mRecursiveMutex;

  public:
    // Acquire/lock this mutex exclusively. The same thread may acquire the
    // mutex multiple times without blocking. The owning thread must release the
    // mutex the same number of times it was acquired.
    void
    Lock() EXCLUSIVE_LOCK_FUNCTION()
    {
        mRecursiveMutex.lock();
    }

    // Release/unlock the mutex. Only the owning thread can release the mutex,
    // and the mutex must be released as many times as it was acquired.
    void
    Unlock() UNLOCK_FUNCTION()
    {
        mRecursiveMutex.unlock();
    }
};

using MutexLocker = MutexLockerT<Mutex>;
using RecursiveMutexLocker = MutexLockerT<RecursiveMutex>;
using SharedLockExclusive = MutexLockerT<SharedMutex>;

#endif // THREAD_ANNOTATIONS_H_