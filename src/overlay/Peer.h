#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h" // IWYU pragma: keep
#include "database/Database.h"
#include "lib/json/json.h"
#include "main/AppConnector.h"
#include "medida/timer.h"
#include "overlay/Hmac.h"
#include "overlay/PeerBareAddress.h"
#include "util/NonCopyable.h"
#include "util/Timer.h"
#include "xdrpp/message.h"

namespace stellar
{

typedef std::shared_ptr<SCPQuorumSet> SCPQuorumSetPtr;

static size_t const MAX_MESSAGE_SIZE = 1024 * 1024 * 16;     // 16 MB
static size_t const MAX_TX_SET_ALLOWANCE = 1024 * 1024 * 10; // 10 MB
static size_t const MAX_SOROBAN_BYTE_ALLOWANCE =
    MAX_TX_SET_ALLOWANCE / 2; // 5 MB
static size_t const MAX_CLASSIC_BYTE_ALLOWANCE =
    MAX_TX_SET_ALLOWANCE / 2; // 5 MB

static_assert(MAX_TX_SET_ALLOWANCE >=
              MAX_SOROBAN_BYTE_ALLOWANCE + MAX_CLASSIC_BYTE_ALLOWANCE);

// max tx size is 100KB
static const uint32_t MAX_CLASSIC_TX_SIZE_BYTES = 100 * 1024;

class Application;
class LoopbackPeer;
struct OverlayMetrics;
class FlowControl;
class TxAdverts;
class CapacityTrackedMessage;

// Peer class represents a connected peer (either inbound or outbound)
//
// Connection steps:
//   A initiates a TCP connection to B
//   Once the connection is established, A sends HELLO(CertA,NonceA)
//     HELLO message includes A's listening port and ledger information
//   B now has IP and listening port of A, sends HELLO(CertB,NonceB) back
//   A sends AUTH(signed([seq=0], keyAB))
//     Peers use `seq` counter to prevent message replays
//   B verifies A's AUTH message and does the following:
//     sends AUTH(signed([seq=0], keyBA)) back
//     sends a list of other peers to try
//     maybe disconnects (if no connection slots are available)
//
// keyAB and keyBA are per-connection HMAC keys derived from non-interactive
// ECDH on random curve25519 keys conveyed in CertA and CertB (certs signed by
// Node Ed25519 keys) the result of which is then fed through HKDF with the
// per-connection nonces. See PeerAuth.h.
//
// If any verify step fails, the peer disconnects immediately.

class Peer : public std::enable_shared_from_this<Peer>,
             public NonMovableOrCopyable
{

  public:
    static constexpr std::chrono::seconds PEER_SEND_MODE_IDLE_TIMEOUT =
        std::chrono::seconds(60);
    static constexpr std::chrono::nanoseconds PEER_METRICS_DURATION_UNIT =
        std::chrono::milliseconds(1);
    static constexpr std::chrono::nanoseconds PEER_METRICS_RATE_UNIT =
        std::chrono::seconds(1);

    // The reporting will be based on the previous
    // PEER_METRICS_WINDOW_SIZE-second time window.
    static constexpr std::chrono::seconds PEER_METRICS_WINDOW_SIZE =
        std::chrono::seconds(300);

    typedef std::shared_ptr<Peer> pointer;

    enum PeerState
    {
        CONNECTING = 0,
        CONNECTED = 1,
        GOT_HELLO = 2,
        GOT_AUTH = 3,
        CLOSING = 4
    };

    struct QueryInfo
    {
        VirtualClock::time_point mLastTimeStamp;
        uint32_t mNumQueries{0};
    };

    static inline int
    format_as(PeerState const& s)
    {
        return static_cast<int>(s);
    }

    enum PeerRole
    {
        REMOTE_CALLED_US,
        WE_CALLED_REMOTE
    };

    static inline std::string
    format_as(PeerRole const& r)
    {
        return (r == REMOTE_CALLED_US) ? "REMOTE_CALLED_US"
                                       : "WE_CALLED_REMOTE";
    }

    enum class DropDirection
    {
        REMOTE_DROPPED_US,
        WE_DROPPED_REMOTE
    };

    struct PeerMetrics
    {
        PeerMetrics(VirtualClock::time_point connectedTime);
        std::atomic<uint64_t> mMessageRead;
        std::atomic<uint64_t> mMessageWrite;
        std::atomic<uint64_t> mByteRead;
        std::atomic<uint64_t> mByteWrite;
        std::atomic<uint64_t> mAsyncRead;
        std::atomic<uint64_t> mAsyncWrite;
        std::atomic<uint64_t> mMessageDrop;

        medida::Timer mMessageDelayInWriteQueueTimer;
        medida::Timer mMessageDelayInAsyncWriteTimer;
        medida::Timer mAdvertQueueDelay;
        medida::Timer mPullLatency;

        std::atomic<uint64_t> mDemandTimeouts;
        std::atomic<uint64_t> mUniqueFloodBytesRecv;
        std::atomic<uint64_t> mDuplicateFloodBytesRecv;
        std::atomic<uint64_t> mUniqueFetchBytesRecv;
        std::atomic<uint64_t> mDuplicateFetchBytesRecv;

        std::atomic<uint64_t> mUniqueFloodMessageRecv;
        std::atomic<uint64_t> mDuplicateFloodMessageRecv;
        std::atomic<uint64_t> mUniqueFetchMessageRecv;
        std::atomic<uint64_t> mDuplicateFetchMessageRecv;

        std::atomic<uint64_t> mTxHashReceived;
        std::atomic<uint64_t> mTxDemandSent;

        std::atomic<VirtualClock::time_point> mConnectedTime;

        std::atomic<uint64_t> mMessagesFulfilled;
        std::atomic<uint64_t> mBannedMessageUnfulfilled;
        std::atomic<uint64_t> mUnknownMessageUnfulfilled;
    };

    struct TimestampedMessage
    {
        VirtualClock::time_point mEnqueuedTime;
        VirtualClock::time_point mIssuedTime;
        VirtualClock::time_point mCompletedTime;
        void recordWriteTiming(OverlayMetrics& metrics,
                               PeerMetrics& peerMetrics);
        xdr::msg_ptr mMessage;
    };

    // NB: all Peer's protected state should have some synchronization
    // mechanisms on it: either be const, or atomic, or associated with a lock,
    // or hidden in a helper class that does one of the same. Some methods in
    // subclasses of Peer will run on a background thread, so may try to access
    // the protected state. Peer state lacking synchronization should be moved
    // to the private section below.
  protected:
    AppConnector& mAppConnector;

    Hash const mNetworkID;
    std::shared_ptr<FlowControl> mFlowControl;
    std::atomic<VirtualClock::time_point> mLastRead;
    std::atomic<VirtualClock::time_point> mLastWrite;
    std::atomic<VirtualClock::time_point> mEnqueueTimeOfLastWrite;

    PeerRole const mRole;
    OverlayMetrics& mOverlayMetrics;
    PeerMetrics mPeerMetrics;

    // Mutex to protect PeerState, which can be accessed and modified from
    // multiple threads
#ifndef USE_TRACY
    std::recursive_mutex mutable mStateMutex;
#else
    mutable TracyLockable(std::recursive_mutex, mStateMutex);
#endif

    Hmac mHmac;
    // Does local node have capacity to read from this peer
    bool canRead() const;
    // helper method to acknowledge that some bytes were received
    void receivedBytes(size_t byteCount, bool gotFullMessage);
    virtual bool
    useBackgroundThread() const
    {
        return mAppConnector.getConfig().BACKGROUND_OVERLAY_PROCESSING;
    }

    void initialize(PeerBareAddress const& address);
    void shutdownAndRemovePeer(std::string const& reason,
                               DropDirection dropDirection);

    // Subclasses should only use these methods to access peer state;
    // they all take a LockGuard that should be holding mStateMutex,
    // but do not lock that mutex themselves (to allow atomic
    // read-modify-write cycles or similar patterns in callers).
    bool shouldAbort(RecursiveLockGuard const& stateGuard) const;
    void setState(RecursiveLockGuard const& stateGuard, PeerState newState);
    PeerState
    getState(RecursiveLockGuard const& stateGuard) const
    {
        return mState;
    }

    bool recvAuthenticatedMessage(AuthenticatedMessage&& msg);
    // These exist mostly to be overridden in TCPPeer and callable via
    // shared_ptr<Peer> as a captured shared_from_this().
    virtual void connectHandler(asio::error_code const& ec);

    // If parallel processing is enabled, execute this function in the
    // background. Otherwise, synchronously execute on the main thread.
    void maybeExecuteInBackground(std::string const& jobName,
                                  std::function<void(std::shared_ptr<Peer>)> f);
    VirtualTimer&
    getRecurrentTimer()
    {
        releaseAssert(threadIsMain());
        return mRecurringTimer;
    }

    // NB: Everything below is private to minimize the chance that subclasses
    // with methods running on background threads might access this
    // unsynchronized state. All methods that access this private state should
    // assert that they are running on the main
    // IOW, all methods using these private variables and functions below must
    // synchronize access manually
  private:
    PeerState mState;
    NodeID mPeerID;
    uint256 mSendNonce;
    uint256 mRecvNonce;

    std::string mRemoteVersion;
    uint32_t mRemoteOverlayMinVersion;
    uint32_t mRemoteOverlayVersion;
    PeerBareAddress mAddress;

    VirtualClock::time_point mCreationTime;
    VirtualTimer mRecurringTimer;
    VirtualTimer mDelayedExecutionTimer;

    std::shared_ptr<TxAdverts> mTxAdverts;
    QueryInfo mQSetQueryInfo;
    QueryInfo mTxSetQueryInfo;
    bool mPeersReceived{false};

    static Hash pingIDfromTimePoint(VirtualClock::time_point const& tp);
    void pingPeer();
    void maybeProcessPingResponse(Hash const& id);
    VirtualClock::time_point mPingSentTime;
    std::chrono::milliseconds mLastPing;

    void recvRawMessage(std::shared_ptr<CapacityTrackedMessage> msgTracker);

    virtual void recvError(StellarMessage const& msg);
    void updatePeerRecordAfterEcho();
    void updatePeerRecordAfterAuthentication();
    void recvAuth(StellarMessage const& msg);
    void recvDontHave(StellarMessage const& msg);
    void recvHello(Hello const& elo);
    void recvPeers(StellarMessage const& msg);
    void recvSurveyRequestMessage(StellarMessage const& msg);
    void recvSurveyResponseMessage(StellarMessage const& msg);
    void recvSurveyStartCollectingMessage(StellarMessage const& msg);
    void recvSurveyStopCollectingMessage(StellarMessage const& msg);
    void recvSendMore(StellarMessage const& msg);

    void recvGetTxSet(StellarMessage const& msg);
    void recvTxSet(StellarMessage const& msg);
    void recvGeneralizedTxSet(StellarMessage const& msg);
    void recvTransaction(CapacityTrackedMessage const& msgTracker);
    void recvGetSCPQuorumSet(StellarMessage const& msg);
    void recvSCPQuorumSet(StellarMessage const& msg);
    void recvSCPMessage(CapacityTrackedMessage const& msgTracker);
    void recvGetSCPState(StellarMessage const& msg);
    void recvFloodAdvert(StellarMessage const& msg);
    void recvFloodDemand(StellarMessage const& msg);

    void sendHello();
    void sendAuth();
    void sendSCPQuorumSet(SCPQuorumSetPtr qSet);
    void sendDontHave(MessageType type, uint256 const& itemID);
    void sendPeers();
    void sendError(ErrorCode error, std::string const& message);
    bool process(QueryInfo& queryInfo);

    void recvMessage(std::shared_ptr<CapacityTrackedMessage> msgTracker);

    // NB: This is a move-argument because the write-buffer has to travel
    // with the write-request through the async IO system, and we might have
    // several queued at once. We have carefully arranged this to not copy
    // data more than the once necessary into this buffer, but it can't be
    // put in a reused/non-owned buffer without having to buffer/queue
    // messages somewhere else. The async write request will point _into_
    // this owned buffer. This is really the best we can do.
    virtual void sendMessage(xdr::msg_ptr&& xdrBytes) = 0;
    virtual void scheduleRead() = 0;
    virtual void
    connected()
    {
    }

    virtual AuthCert getAuthCert();

    void startRecurrentTimer();

    void recurrentTimerExpired(asio::error_code const& error);
    std::chrono::seconds getIOTimeout() const;

    void sendAuthenticatedMessage(
        std::shared_ptr<StellarMessage const> msg,
        std::optional<VirtualClock::time_point> timePlaced = std::nullopt);
    void beginMessageProcessing(StellarMessage const& msg);
    void endMessageProcessing(StellarMessage const& msg);

  public:
    /* The following functions must all be called from the main thread (they all
     * contain releaseAssert(threadIsMain())) */
    Peer(Application& app, PeerRole role);

    void cancelTimers();

    std::string msgSummary(StellarMessage const& stellarMsg);
    void sendGetTxSet(uint256 const& setID);
    void sendGetQuorumSet(uint256 const& setID);
    void sendGetScpState(uint32 ledgerSeq);
    void sendErrorAndDrop(ErrorCode error, std::string const& message);
    void sendTxDemand(TxDemandVector&& demands);
    // Queue up an advert to send, return true if the advert was queued, and
    // false otherwise (if advert is a duplicate, for example)
    bool sendAdvert(Hash const& txHash);
    void sendSendMore(uint32_t numMessages, uint32_t numBytes);

    virtual void sendMessage(std::shared_ptr<StellarMessage const> msg,
                             bool log = true);

    PeerRole
    getRole() const
    {
        releaseAssert(threadIsMain());
        return mRole;
    }

    std::chrono::seconds getLifeTime() const;
    std::chrono::milliseconds getPing() const;

    std::string const&
    getRemoteVersion() const
    {
        releaseAssert(threadIsMain());
        return mRemoteVersion;
    }

    uint32_t
    getRemoteOverlayVersion() const
    {
        releaseAssert(threadIsMain());
        return mRemoteOverlayVersion;
    }

    PeerBareAddress const&
    getAddress()
    {
        releaseAssert(threadIsMain());
        return mAddress;
    }

    NodeID
    getPeerID() const
    {
        releaseAssert(threadIsMain());
        return mPeerID;
    }

    std::string const& toString();

    void startExecutionDelayedTimer(
        VirtualClock::duration d, std::function<void()> const& onSuccess,
        std::function<void(asio::error_code)> const& onFailure);
    Json::Value getJsonInfo(bool compact) const;
    void handleMaxTxSizeIncrease(uint32_t increase);
    virtual ~Peer()
    {
        releaseAssert(threadIsMain());
    }

    // Pull Mode facade methods , must be called from the main thread.
    // Queue up transaction hashes for processing again. This method is normally
    // called if a previous demand failed or timed out.
    void retryAdvert(std::list<Hash>& hashes);
    // Does this peer have any transaction hashes to process?
    bool hasAdvert();
    // Pop the next transaction hash to process
    std::pair<Hash, std::optional<VirtualClock::time_point>> popAdvert();
    // Clear pull mode state below `ledgerSeq`
    void clearBelow(uint32_t ledgerSeq);

    /* The following functions can be called from background thread, so they
     * must be thread-safe */
    bool isConnected(RecursiveLockGuard const& stateGuard) const;
    bool isAuthenticated(RecursiveLockGuard const& stateGuard) const;

    PeerMetrics&
    getPeerMetrics()
    {
        // PeerMetrics is thread-safe
        return mPeerMetrics;
    }

    PeerMetrics const&
    getPeerMetrics() const
    {
        return mPeerMetrics;
    }
    virtual void drop(std::string const& reason,
                      DropDirection dropDirection) = 0;

    friend class LoopbackPeer;
    friend class PeerStub;
    friend class CapacityTrackedMessage;

#ifdef BUILD_TESTS
    std::shared_ptr<FlowControl>
    getFlowControl() const
    {
        return mFlowControl;
    }
    bool isAuthenticatedForTesting() const;
    bool shouldAbortForTesting() const;
    bool isConnectedForTesting() const;
    void
    sendAuthenticatedMessageForTesting(
        std::shared_ptr<StellarMessage const> msg)
    {
        sendAuthenticatedMessage(std::move(msg));
    }
    void
    sendXdrMessageForTesting(xdr::msg_ptr xdrBytes)
    {
        sendMessage(std::move(xdrBytes));
    }
    std::string mDropReason;
#endif

    // Public thread-safe methods that access Peer's state
    void
    assertAuthenticated() const
    {
        RecursiveLockGuard guard(mStateMutex);
        releaseAssert(isAuthenticated(guard));
    }

    void
    assertShuttingDown() const
    {
        RecursiveLockGuard guard(mStateMutex);
        releaseAssert(mState == CLOSING);
    }

    // equivalent to isAuthenticated being an atomic flag; i.e. it can be safely
    // loaded, but once it's loaded, there are no guarantees on its value. If
    // the code block depends on isAuthenticated value being constant, use
    // `doIfAuthenticated`
    bool
    isAuthenticatedAtomic() const
    {
        RecursiveLockGuard guard(mStateMutex);
        return isAuthenticated(guard);
    }

    void
    doIfAuthenticated(std::function<void()> f)
    {
        RecursiveLockGuard guard(mStateMutex);
        if (isAuthenticated(guard))
        {
            f();
        }
    }
};

// CapacityTrackedMessage is a helper class to track when the message is done
// being processed by core using RAII. On destruction, it will automatically
// signal completion to Peer. This allows Peer to track available capacity, and
// request more traffic. CapacityTrackedMessage also optionally stores a BLAKE2
// hash of the message, so overlay can decide if a duplicate message can be
// dropped as early as possible. Note: this class has side effects;
// specifically, it may trigger a send of SEND_MORE message on destruction
class CapacityTrackedMessage : private NonMovableOrCopyable
{
    std::weak_ptr<Peer> const mWeakPeer;
    StellarMessage const mMsg;
    std::optional<Hash> mMaybeHash;

  public:
    CapacityTrackedMessage(std::weak_ptr<Peer> peer, StellarMessage const& msg);
    StellarMessage const& getMessage() const;
    ~CapacityTrackedMessage();
    std::optional<Hash> maybeGetHash() const;
};
}
