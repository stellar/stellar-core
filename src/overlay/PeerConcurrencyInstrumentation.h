#ifndef PEER_CONCURRENCY_INSTRUMENTATION_H
#define PEER_CONCURRENCY_INSTRUMENTATION_H

// =============================================================================
// Overlay Peer Concurrency Instrumentation
// =============================================================================
//
// Three instrumentation tools for diagnosing overlay "stuck" conditions:
//
// Tool 1: Lock-Order Checker
//   - Records per-thread lock acquisition order at runtime
//   - Detects AB/BA violations across all three overlay locks
//     (mStateMutex, mFlowControlMutex, mMutex/Hmac)
//   - Reports violations with lock names and thread IDs
//
// Tool 2: Write Queue Depth + Outbound Capacity Leak Detector
//   - Tracks write queue depth per peer over time (histogram buckets)
//   - Monitors outbound capacity deltas to detect capacity leaks
//     caused by queue trim racing with in-flight async_write
//   - Reports when capacity decreases without a corresponding SEND_MORE
//
// Tool 3: Handler Duration Logger with Stall Detection
//   - Times every overlay handler (read/write/connect/recv)
//   - Detects stalls when a handler exceeds configurable threshold
//   - Logs slow handlers with thread ID and handler name
//   - Tracks throttle durations for read-side flow control
//
// Enable with: -DPEER_DEBUG_INSTRUMENTATION
// All instrumentation is zero-cost when disabled (macros expand to void).
// =============================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef PEER_DEBUG_INSTRUMENTATION

namespace stellar
{
namespace overlay_instrumentation
{

// =========================================================================
// Tool 1: Lock-Order Checker
// =========================================================================
//
// Enforces a global lock ordering at runtime:
//   mStateMutex (order=1) > mFlowControlMutex (order=2) > Hmac::mMutex
//   (order=3)
//
// If a thread acquires a lock with order N while holding a lock with
// order M > N, a violation is recorded. This catches AB/BA patterns that
// could lead to deadlock.
//
// Usage:
//   PEER_LOCK_ORDER_ACQUIRE("mStateMutex", 1, &mutex);
//   ... critical section ...
//   PEER_LOCK_ORDER_RELEASE(&mutex);

class LockOrderChecker
{
  public:
    static constexpr size_t MAX_VIOLATIONS = 64;

    struct LockOrderViolation
    {
        const char* heldLockName;
        int heldLockOrder;
        const char* acquiredLockName;
        int acquiredLockOrder;
        std::thread::id threadId;
        std::chrono::steady_clock::time_point timestamp;
    };

    static LockOrderChecker&
    instance()
    {
        static LockOrderChecker inst;
        return inst;
    }

    void
    recordAcquire(const char* lockName, int lockOrder, void* lockAddr)
    {
        if (!mEnabled.load(std::memory_order_relaxed))
            return;

        auto tid = std::this_thread::get_id();

        // Check ordering against currently held locks on this thread
        {
            std::lock_guard<std::mutex> g(mMutex);
            auto& held = mThreadHeldLocks[tid];
            for (auto const& h : held)
            {
                if (h.order > lockOrder)
                {
                    // Violation: acquiring a lower-order lock while holding
                    // a higher-order one
                    size_t idx =
                        mViolationCount.fetch_add(1, std::memory_order_relaxed);
                    if (idx < MAX_VIOLATIONS)
                    {
                        auto& v = mViolations[idx];
                        v.heldLockName = h.name;
                        v.heldLockOrder = h.order;
                        v.acquiredLockName = lockName;
                        v.acquiredLockOrder = lockOrder;
                        v.threadId = tid;
                        v.timestamp = std::chrono::steady_clock::now();
                    }
                }
            }
            held.push_back({lockName, lockOrder, lockAddr});
        }
    }

    void
    recordRelease(void* lockAddr)
    {
        if (!mEnabled.load(std::memory_order_relaxed))
            return;

        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> g(mMutex);
        auto it = mThreadHeldLocks.find(tid);
        if (it != mThreadHeldLocks.end())
        {
            auto& held = it->second;
            held.erase(std::remove_if(held.begin(), held.end(),
                                      [lockAddr](auto const& h) {
                                          return h.addr == lockAddr;
                                      }),
                       held.end());
        }
    }

    size_t
    getViolationCount() const
    {
        return std::min(mViolationCount.load(std::memory_order_relaxed),
                        MAX_VIOLATIONS);
    }

    std::string
    getViolationReport()
    {
        std::ostringstream oss;
        size_t count = getViolationCount();
        oss << "=== Lock Order Violations: " << count << " ===\n";
        for (size_t i = 0; i < count; ++i)
        {
            auto const& v = mViolations[i];
            oss << "  Violation " << i << ": thread " << v.threadId << " held "
                << v.heldLockName << " (order=" << v.heldLockOrder
                << "), acquired " << v.acquiredLockName
                << " (order=" << v.acquiredLockOrder << ")\n";
        }
        return oss.str();
    }

    void
    enable()
    {
        mEnabled.store(true, std::memory_order_relaxed);
    }
    void
    disable()
    {
        mEnabled.store(false, std::memory_order_relaxed);
    }
    void
    reset()
    {
        mViolationCount.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> g(mMutex);
        mThreadHeldLocks.clear();
    }

  private:
    LockOrderChecker() : mViolationCount(0), mEnabled(false)
    {
    }

    struct HeldLock
    {
        const char* name;
        int order;
        void* addr;
    };

    std::mutex mMutex;
    std::unordered_map<std::thread::id, std::vector<HeldLock>> mThreadHeldLocks;
    std::atomic<size_t> mViolationCount;
    std::array<LockOrderViolation, MAX_VIOLATIONS> mViolations;
    std::atomic<bool> mEnabled;
};

// =========================================================================
// Tool 2: Write Queue Depth + Outbound Capacity Leak Detector
// =========================================================================
//
// Tracks write queue depth in histogram buckets and detects outbound
// capacity leaks caused by queue trims racing with in-flight async_write.
//
// The capacity leak detection works by tracking:
//   - Total capacity locked by getNextBatchToSend
//   - Total capacity released by processSentMessages
//   - Total capacity released by SEND_MORE_EXTENDED
//   - Net capacity should never decrease outside of locking
//
// Usage:
//   PEER_WRITE_QUEUE_DEPTH_SAMPLE(peerAddr, depth);
//   PEER_OUTBOUND_CAPACITY_LOCKED(peerAddr, amount);
//   PEER_OUTBOUND_CAPACITY_RELEASED(peerAddr, amount);
//   PEER_QUEUE_TRIM_EVENT(peerAddr, trimmedCount, hadInFlight);

class WriteQueueAndCapacityTracker
{
  public:
    static constexpr size_t HISTOGRAM_BUCKETS = 8;
    // Bucket boundaries: 0, 1-4, 5-16, 17-64, 65-256, 257-1024, 1025-4096,
    // 4097+
    static constexpr size_t BUCKET_BOUNDS[HISTOGRAM_BUCKETS] = {
        0, 1, 5, 17, 65, 257, 1025, 4097};

    static constexpr size_t MAX_CAPACITY_EVENTS = 256;

    struct CapacityEvent
    {
        enum Type
        {
            LOCKED,
            RELEASED_SENT,
            RELEASED_SEND_MORE,
            TRIM_WITH_INFLIGHT,
            TRIM_NO_INFLIGHT
        };
        Type type;
        uint64_t amount;
        std::chrono::steady_clock::time_point timestamp;
    };

    struct PerPeerState
    {
        // Write queue depth histogram
        std::array<std::atomic<uint64_t>, HISTOGRAM_BUCKETS> depthHistogram{};
        std::atomic<size_t> maxDepthSeen{0};
        std::atomic<size_t> currentDepth{0};

        // Capacity tracking
        std::atomic<uint64_t> totalCapacityLocked{0};
        std::atomic<uint64_t> totalCapacityReleasedSent{0};
        std::atomic<uint64_t> totalCapacityReleasedSendMore{0};
        std::atomic<uint64_t> trimWithInflightCount{0};

        PerPeerState()
        {
            for (auto& b : depthHistogram)
                b.store(0);
        }
    };

    static WriteQueueAndCapacityTracker&
    instance()
    {
        static WriteQueueAndCapacityTracker inst;
        return inst;
    }

    void
    recordDepth(const void* peerAddr, size_t depth)
    {
        if (!mEnabled.load(std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> g(mMutex);
        auto& state = mPeerStates[peerAddr];
        state.currentDepth.store(depth);
        if (depth > state.maxDepthSeen.load())
            state.maxDepthSeen.store(depth);

        // Find histogram bucket
        size_t bucket = HISTOGRAM_BUCKETS - 1;
        for (size_t i = 0; i < HISTOGRAM_BUCKETS - 1; ++i)
        {
            if (depth < BUCKET_BOUNDS[i + 1])
            {
                bucket = i;
                break;
            }
        }
        state.depthHistogram[bucket].fetch_add(1, std::memory_order_relaxed);
    }

    void
    recordCapacityLocked(const void* peerAddr, uint64_t amount)
    {
        if (!mEnabled.load(std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> g(mMutex);
        auto& state = mPeerStates[peerAddr];
        state.totalCapacityLocked.fetch_add(amount, std::memory_order_relaxed);
    }

    void
    recordCapacityReleasedSent(const void* peerAddr, uint64_t amount)
    {
        if (!mEnabled.load(std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> g(mMutex);
        auto& state = mPeerStates[peerAddr];
        state.totalCapacityReleasedSent.fetch_add(amount,
                                                  std::memory_order_relaxed);
    }

    void
    recordQueueTrim(const void* peerAddr, size_t trimmed, bool hadInFlight)
    {
        if (!mEnabled.load(std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> g(mMutex);
        auto& state = mPeerStates[peerAddr];
        if (hadInFlight)
        {
            state.trimWithInflightCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool
    hasCapacityLeak(const void* peerAddr)
    {
        std::lock_guard<std::mutex> g(mMutex);
        auto it = mPeerStates.find(peerAddr);
        if (it == mPeerStates.end())
            return false;
        auto& state = it->second;
        auto locked = state.totalCapacityLocked.load();
        auto released = state.totalCapacityReleasedSent.load();
        // If we've locked more than released, and there are trim events
        // with in-flight messages, we likely have a capacity leak
        return (locked > released) && (state.trimWithInflightCount.load() > 0);
    }

    std::string
    getReport()
    {
        std::ostringstream oss;
        std::lock_guard<std::mutex> g(mMutex);
        oss << "=== Write Queue & Capacity Report ===\n";
        for (auto const& kv : mPeerStates)
        {
            auto const& state = kv.second;
            oss << "Peer " << kv.first << ":\n";
            oss << "  Current depth: " << state.currentDepth.load()
                << ", max: " << state.maxDepthSeen.load() << "\n";
            oss << "  Depth histogram: [";
            for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i)
            {
                if (i > 0)
                    oss << ", ";
                oss << state.depthHistogram[i].load();
            }
            oss << "]\n";
            oss << "  Capacity locked: " << state.totalCapacityLocked.load()
                << ", released(sent): "
                << state.totalCapacityReleasedSent.load()
                << ", released(SEND_MORE): "
                << state.totalCapacityReleasedSendMore.load() << "\n";
            oss << "  Trims with in-flight: "
                << state.trimWithInflightCount.load() << "\n";
            if (state.totalCapacityLocked.load() >
                state.totalCapacityReleasedSent.load())
            {
                oss << "  ** POTENTIAL CAPACITY LEAK: locked > released **\n";
            }
        }
        return oss.str();
    }

    void
    enable()
    {
        mEnabled.store(true, std::memory_order_relaxed);
    }
    void
    disable()
    {
        mEnabled.store(false, std::memory_order_relaxed);
    }
    void
    reset()
    {
        std::lock_guard<std::mutex> g(mMutex);
        mPeerStates.clear();
    }

  private:
    WriteQueueAndCapacityTracker() : mEnabled(false)
    {
    }

    std::mutex mMutex;
    std::unordered_map<const void*, PerPeerState> mPeerStates;
    std::atomic<bool> mEnabled;
};

// =========================================================================
// Tool 3: Handler Duration Logger with Stall Detection
// =========================================================================
//
// Times every overlay ASIO handler and detects stalls. A "stall" is
// defined as any single handler invocation exceeding a configurable
// threshold (default 1 second). This catches:
//   - Main thread monopolization by message processing
//   - Slow async_write completions due to TCP backpressure
//   - Read throttle durations (time between throttle and stopThrottling)
//
// Usage:
//   PEER_HANDLER_DURATION_ENTER("writeHandler");
//   ... handler body ...
//   PEER_HANDLER_DURATION_EXIT("writeHandler");
//
//   // Or use the RAII guard:
//   PEER_HANDLER_DURATION_GUARD("writeHandler");

class HandlerDurationLogger
{
  public:
    static constexpr uint64_t DEFAULT_STALL_THRESHOLD_US = 1'000'000; // 1s
    static constexpr size_t MAX_STALL_EVENTS = 128;

    struct StallEvent
    {
        const char* handlerName;
        uint64_t durationUs;
        std::thread::id threadId;
        std::chrono::steady_clock::time_point timestamp;
    };

    struct HandlerStats
    {
        std::atomic<uint64_t> invocationCount{0};
        std::atomic<uint64_t> totalDurationUs{0};
        std::atomic<uint64_t> maxDurationUs{0};
        std::atomic<uint64_t> stallCount{0};
    };

    static HandlerDurationLogger&
    instance()
    {
        static HandlerDurationLogger inst;
        return inst;
    }

    void
    enter(const char* name)
    {
        if (!mEnabled.load(std::memory_order_relaxed))
            return;

        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> g(mMutex);
        mActiveHandlers[{tid, name}] = std::chrono::steady_clock::now();
    }

    void
    exit(const char* name)
    {
        if (!mEnabled.load(std::memory_order_relaxed))
            return;

        auto now = std::chrono::steady_clock::now();
        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> g(mMutex);

        auto key = std::make_pair(tid, name);
        auto it = mActiveHandlers.find(key);
        if (it == mActiveHandlers.end())
            return;

        auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              now - it->second)
                              .count();
        mActiveHandlers.erase(it);

        // Update stats
        auto& stats = mHandlerStats[name];
        stats.invocationCount.fetch_add(1, std::memory_order_relaxed);
        stats.totalDurationUs.fetch_add(durationUs, std::memory_order_relaxed);

        uint64_t prevMax = stats.maxDurationUs.load(std::memory_order_relaxed);
        while (static_cast<uint64_t>(durationUs) > prevMax)
        {
            if (stats.maxDurationUs.compare_exchange_weak(prevMax, durationUs))
                break;
        }

        // Check for stall
        if (static_cast<uint64_t>(durationUs) >
            mStallThresholdUs.load(std::memory_order_relaxed))
        {
            stats.stallCount.fetch_add(1, std::memory_order_relaxed);
            size_t idx = mStallCount.fetch_add(1, std::memory_order_relaxed);
            if (idx < MAX_STALL_EVENTS)
            {
                auto& evt = mStallEvents[idx];
                evt.handlerName = name;
                evt.durationUs = durationUs;
                evt.threadId = tid;
                evt.timestamp = now;
            }
        }
    }

    void
    recordThrottleDuration(const char* peerName, uint64_t durationUs)
    {
        if (!mEnabled.load(std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> g(mMutex);
        auto& stats = mHandlerStats["throttle_read"];
        stats.invocationCount.fetch_add(1, std::memory_order_relaxed);
        stats.totalDurationUs.fetch_add(durationUs, std::memory_order_relaxed);

        uint64_t prevMax = stats.maxDurationUs.load(std::memory_order_relaxed);
        while (durationUs > prevMax)
        {
            if (stats.maxDurationUs.compare_exchange_weak(prevMax, durationUs))
                break;
        }

        if (durationUs > mStallThresholdUs.load(std::memory_order_relaxed))
        {
            stats.stallCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::string
    getReport()
    {
        std::ostringstream oss;
        std::lock_guard<std::mutex> g(mMutex);
        oss << "=== Handler Duration Report ===\n";
        for (auto const& kv : mHandlerStats)
        {
            auto const& stats = kv.second;
            auto count = stats.invocationCount.load();
            if (count == 0)
                continue;
            auto totalUs = stats.totalDurationUs.load();
            oss << "  " << kv.first << ": count=" << count
                << " avg=" << (totalUs / count) << "us"
                << " max=" << stats.maxDurationUs.load() << "us"
                << " stalls=" << stats.stallCount.load() << "\n";
        }

        size_t stallCount = std::min(mStallCount.load(), MAX_STALL_EVENTS);
        if (stallCount > 0)
        {
            oss << "\n  Recent stalls (" << stallCount << " total):\n";
            for (size_t i = 0; i < stallCount; ++i)
            {
                auto const& evt = mStallEvents[i];
                oss << "    " << evt.handlerName << ": " << evt.durationUs
                    << "us on thread " << evt.threadId << "\n";
            }
        }
        return oss.str();
    }

    void
    setStallThreshold(uint64_t thresholdUs)
    {
        mStallThresholdUs.store(thresholdUs, std::memory_order_relaxed);
    }

    void
    enable()
    {
        mEnabled.store(true, std::memory_order_relaxed);
    }
    void
    disable()
    {
        mEnabled.store(false, std::memory_order_relaxed);
    }
    void
    reset()
    {
        mStallCount.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> g(mMutex);
        mHandlerStats.clear();
        mActiveHandlers.clear();
    }

  private:
    HandlerDurationLogger()
        : mStallThresholdUs(DEFAULT_STALL_THRESHOLD_US)
        , mStallCount(0)
        , mEnabled(false)
    {
    }

    struct PairHash
    {
        size_t
        operator()(std::pair<std::thread::id, const char*> const& p) const
        {
            auto h1 = std::hash<std::thread::id>{}(p.first);
            auto h2 =
                std::hash<const void*>{}(static_cast<const void*>(p.second));
            return h1 ^ (h2 << 1);
        }
    };

    std::mutex mMutex;
    std::unordered_map<std::pair<std::thread::id, const char*>,
                       std::chrono::steady_clock::time_point, PairHash>
        mActiveHandlers;
    std::unordered_map<const char*, HandlerStats> mHandlerStats;
    std::atomic<uint64_t> mStallThresholdUs;
    std::atomic<size_t> mStallCount;
    std::array<StallEvent, MAX_STALL_EVENTS> mStallEvents;
    std::atomic<bool> mEnabled;
};

// RAII guard for handler duration tracking
class HandlerDurationGuard
{
  public:
    explicit HandlerDurationGuard(const char* name) : mName(name)
    {
        HandlerDurationLogger::instance().enter(mName);
    }
    ~HandlerDurationGuard()
    {
        HandlerDurationLogger::instance().exit(mName);
    }

  private:
    const char* mName;
};

// =========================================================================
// Combined diagnostics
// =========================================================================

inline std::string
getFullDiagnostics()
{
    std::ostringstream oss;
    oss << "===========================================================\n";
    oss << "     Overlay Concurrency Instrumentation Report\n";
    oss << "===========================================================\n\n";
    oss << LockOrderChecker::instance().getViolationReport() << "\n";
    oss << WriteQueueAndCapacityTracker::instance().getReport() << "\n";
    oss << HandlerDurationLogger::instance().getReport() << "\n";
    return oss.str();
}

inline void
enableAllInstrumentation()
{
    LockOrderChecker::instance().enable();
    WriteQueueAndCapacityTracker::instance().enable();
    HandlerDurationLogger::instance().enable();
}

inline void
disableAllInstrumentation()
{
    LockOrderChecker::instance().disable();
    WriteQueueAndCapacityTracker::instance().disable();
    HandlerDurationLogger::instance().disable();
}

inline void
resetAllInstrumentation()
{
    LockOrderChecker::instance().reset();
    WriteQueueAndCapacityTracker::instance().reset();
    HandlerDurationLogger::instance().reset();
}

} // namespace overlay_instrumentation
} // namespace stellar

// =========================================================================
// Tool 1 macros: Lock-Order Checker
// =========================================================================
// Lock order values:
//   1 = mStateMutex (highest, acquired first)
//   2 = mFlowControlMutex
//   3 = Hmac::mMutex (lowest, acquired last)
#define PEER_LOCK_ORDER_ACQUIRE(name, order, addr) \
    stellar::overlay_instrumentation::LockOrderChecker::instance() \
        .recordAcquire(name, order, addr)
#define PEER_LOCK_ORDER_RELEASE(addr) \
    stellar::overlay_instrumentation::LockOrderChecker::instance() \
        .recordRelease(addr)

// =========================================================================
// Tool 2 macros: Write Queue Depth + Capacity Tracking
// =========================================================================
#define PEER_WRITE_QUEUE_DEPTH_SAMPLE(peer, depth) \
    stellar::overlay_instrumentation::WriteQueueAndCapacityTracker::instance() \
        .recordDepth(peer, depth)
#define PEER_OUTBOUND_CAPACITY_LOCKED(peer, amount) \
    stellar::overlay_instrumentation::WriteQueueAndCapacityTracker::instance() \
        .recordCapacityLocked(peer, amount)
#define PEER_OUTBOUND_CAPACITY_RELEASED(peer, amount) \
    stellar::overlay_instrumentation::WriteQueueAndCapacityTracker::instance() \
        .recordCapacityReleasedSent(peer, amount)
#define PEER_QUEUE_TRIM_EVENT(peer, count, hadInFlight) \
    stellar::overlay_instrumentation::WriteQueueAndCapacityTracker::instance() \
        .recordQueueTrim(peer, count, hadInFlight)

// =========================================================================
// Tool 3 macros: Handler Duration Logger
// =========================================================================
#define PEER_HANDLER_DURATION_ENTER(name) \
    stellar::overlay_instrumentation::HandlerDurationLogger::instance().enter( \
        name)
#define PEER_HANDLER_DURATION_EXIT(name) \
    stellar::overlay_instrumentation::HandlerDurationLogger::instance().exit( \
        name)
#define PEER_HANDLER_DURATION_GUARD(name) \
    stellar::overlay_instrumentation::HandlerDurationGuard \
    _handler_guard_##__LINE__(name)
#define PEER_THROTTLE_DURATION(peer, durationUs) \
    stellar::overlay_instrumentation::HandlerDurationLogger::instance() \
        .recordThrottleDuration(peer, durationUs)

// =========================================================================
// Legacy macros (backward compatibility)
// =========================================================================
#define PEER_LOCK_ACQUIRE(name, addr) PEER_LOCK_ORDER_ACQUIRE(name, 0, addr)
#define PEER_LOCK_RELEASE(addr) PEER_LOCK_ORDER_RELEASE(addr)
#define PEER_HANDLER_ENTER(name) PEER_HANDLER_DURATION_ENTER(name)
#define PEER_HANDLER_EXIT(name) PEER_HANDLER_DURATION_EXIT(name)
#define PEER_WRITE_QUEUE_DEPTH(depth) \
    PEER_WRITE_QUEUE_DEPTH_SAMPLE(nullptr, depth)
#define PEER_STATE_CHANGE(id, state) ((void)0)

// =========================================================================
// Full diagnostics
// =========================================================================
#define PEER_GET_DIAGNOSTICS() \
    stellar::overlay_instrumentation::getFullDiagnostics()
#define PEER_ENABLE_ALL_INSTRUMENTATION() \
    stellar::overlay_instrumentation::enableAllInstrumentation()
#define PEER_DISABLE_ALL_INSTRUMENTATION() \
    stellar::overlay_instrumentation::disableAllInstrumentation()
#define PEER_RESET_ALL_INSTRUMENTATION() \
    stellar::overlay_instrumentation::resetAllInstrumentation()

#else // !PEER_DEBUG_INSTRUMENTATION

// All macros expand to void when instrumentation is disabled
#define PEER_LOCK_ORDER_ACQUIRE(name, order, addr) ((void)0)
#define PEER_LOCK_ORDER_RELEASE(addr) ((void)0)
#define PEER_WRITE_QUEUE_DEPTH_SAMPLE(peer, depth) ((void)0)
#define PEER_OUTBOUND_CAPACITY_LOCKED(peer, amount) ((void)0)
#define PEER_OUTBOUND_CAPACITY_RELEASED(peer, amount) ((void)0)
#define PEER_QUEUE_TRIM_EVENT(peer, count, hadInFlight) ((void)0)
#define PEER_HANDLER_DURATION_ENTER(name) ((void)0)
#define PEER_HANDLER_DURATION_EXIT(name) ((void)0)
#define PEER_HANDLER_DURATION_GUARD(name) ((void)0)
#define PEER_THROTTLE_DURATION(peer, durationUs) ((void)0)
#define PEER_LOCK_ACQUIRE(name, addr) ((void)0)
#define PEER_LOCK_RELEASE(addr) ((void)0)
#define PEER_HANDLER_ENTER(name) ((void)0)
#define PEER_HANDLER_EXIT(name) ((void)0)
#define PEER_WRITE_QUEUE_DEPTH(depth) ((void)0)
#define PEER_STATE_CHANGE(id, state) ((void)0)
#define PEER_GET_DIAGNOSTICS() std::string("Instrumentation disabled")
#define PEER_ENABLE_ALL_INSTRUMENTATION() ((void)0)
#define PEER_DISABLE_ALL_INSTRUMENTATION() ((void)0)
#define PEER_RESET_ALL_INSTRUMENTATION() ((void)0)

#endif // PEER_DEBUG_INSTRUMENTATION

#endif // PEER_CONCURRENCY_INSTRUMENTATION_H
