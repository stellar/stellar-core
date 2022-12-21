#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "database/Database.h"
#include "lib/json/json.h"
#include "medida/timer.h"
#include "overlay/PeerBareAddress.h"
#include "overlay/StellarXDR.h"
#include "overlay/TxAdvertQueue.h"
#include "util/HashOfHash.h"
#include "util/NonCopyable.h"
#include "util/RandomEvictionCache.h"
#include "util/Timer.h"
#include "xdrpp/message.h"

namespace medida
{
class Timer;
class Meter;
}

namespace stellar
{

typedef std::shared_ptr<SCPQuorumSet> SCPQuorumSetPtr;

class Application;
class LoopbackPeer;
struct OverlayMetrics;

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
    static constexpr uint32_t FIRST_VERSION_SUPPORTING_GENERALIZED_TX_SET = 23;
    static constexpr std::chrono::seconds PEER_SEND_MODE_IDLE_TIMEOUT =
        std::chrono::seconds(60);
    static constexpr std::chrono::nanoseconds PEER_METRICS_DURATION_UNIT =
        std::chrono::milliseconds(1);
    static constexpr std::chrono::nanoseconds PEER_METRICS_RATE_UNIT =
        std::chrono::seconds(1);
    static constexpr uint32_t FIRST_VERSION_REQUIRING_PULL_MODE = 27;

    // The reporting will be based on the previous
    // PEER_METRICS_WINDOW_SIZE-second time window.
    static constexpr std::chrono::seconds PEER_METRICS_WINDOW_SIZE =
        std::chrono::seconds(300);

    bool peerKnowsHash(Hash const& hash);
    void rememberHash(Hash const& hash, uint32_t ledgerSeq);

    typedef std::shared_ptr<Peer> pointer;

    enum PeerState
    {
        CONNECTING = 0,
        CONNECTED = 1,
        GOT_HELLO = 2,
        GOT_AUTH = 3,
        CLOSING = 4
    };

    enum PeerRole
    {
        REMOTE_CALLED_US,
        WE_CALLED_REMOTE
    };

    enum class DropMode
    {
        FLUSH_WRITE_QUEUE,
        IGNORE_WRITE_QUEUE
    };

    enum class DropDirection
    {
        REMOTE_DROPPED_US,
        WE_DROPPED_REMOTE
    };

    struct PeerMetrics
    {
        PeerMetrics(VirtualClock::time_point connectedTime);
        uint64_t mMessageRead;
        uint64_t mMessageWrite;
        uint64_t mByteRead;
        uint64_t mByteWrite;
        uint64_t mAsyncRead;
        uint64_t mAsyncWrite;
        uint64_t mMessageDrop;

        medida::Timer mMessageDelayInWriteQueueTimer;
        medida::Timer mMessageDelayInAsyncWriteTimer;
        medida::Timer mOutboundQueueDelaySCP;
        medida::Timer mOutboundQueueDelayTxs;
        medida::Timer mOutboundQueueDelayAdvert;
        medida::Timer mOutboundQueueDelayDemand;
        medida::Timer mAdvertQueueDelay;
        medida::Timer mPullLatency;

        uint64_t mDemandTimeouts;
        uint64_t mUniqueFloodBytesRecv;
        uint64_t mDuplicateFloodBytesRecv;
        uint64_t mUniqueFetchBytesRecv;
        uint64_t mDuplicateFetchBytesRecv;

        uint64_t mUniqueFloodMessageRecv;
        uint64_t mDuplicateFloodMessageRecv;
        uint64_t mUniqueFetchMessageRecv;
        uint64_t mDuplicateFetchMessageRecv;

        uint64_t mTxHashReceived;
        uint64_t mTxDemandSent;

        VirtualClock::time_point mConnectedTime;

        uint64_t mMessagesFulfilled;
        uint64_t mBannedMessageUnfulfilled;
        uint64_t mUnknownMessageUnfulfilled;
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

    struct QueuedOutboundMessage
    {
        std::shared_ptr<StellarMessage const> mMessage;
        VirtualClock::time_point mTimeEmplaced;
    };

    Json::Value getFlowControlJsonInfo(bool compact) const;
    Json::Value getJsonInfo(bool compact) const;

  protected:
    Application& mApp;

    PeerRole mRole;
    PeerState mState;
    NodeID mPeerID;
    uint256 mSendNonce;
    uint256 mRecvNonce;

    class MsgCapacityTracker : private NonMovableOrCopyable
    {
        std::weak_ptr<Peer> mWeakPeer;
        StellarMessage mMsg;

      public:
        MsgCapacityTracker(std::weak_ptr<Peer> peer, StellarMessage const& msg);
        ~MsgCapacityTracker();
        StellarMessage const& getMessage();
        std::weak_ptr<Peer> getPeer();
    };

    struct ReadingCapacity
    {
        uint64_t mFloodCapacity;
        uint64_t mTotalCapacity;
    };

    // Outbound queues indexes by priority
    // Priority 0 - SCP messages
    // Priority 1 - transactions
    // Priority 2 - flood demands
    // Priority 3 - flood adverts
    std::array<std::deque<QueuedOutboundMessage>, 4> mOutboundQueues;

    // This methods drops obsolete load from the outbound queue
    void addMsgAndMaybeTrimQueue(std::shared_ptr<StellarMessage const> msg);

    // How many flood messages have we received and processed since sending
    // SEND_MORE to this peer
    uint64_t mFloodMsgsProcessed{0};

    // How many flood messages can we send to this peer
    uint64_t mOutboundCapacity{0};

    // Is this peer currently throttled due to lack of capacity
    bool mIsPeerThrottled{false};

    // Does local node have capacity to read from this peer
    bool hasReadingCapacity() const;

    HmacSha256Key mSendMacKey;
    HmacSha256Key mRecvMacKey;
    uint64_t mSendMacSeq{0};
    uint64_t mRecvMacSeq{0};

    std::string mRemoteVersion;
    uint32_t mRemoteOverlayMinVersion;
    uint32_t mRemoteOverlayVersion;
    PeerBareAddress mAddress;

    VirtualClock::time_point mCreationTime;

    VirtualTimer mRecurringTimer;
    VirtualClock::time_point mLastRead;
    VirtualClock::time_point mLastWrite;
    std::optional<VirtualClock::time_point> mNoOutboundCapacity;
    VirtualClock::time_point mEnqueueTimeOfLastWrite;

    static Hash pingIDfromTimePoint(VirtualClock::time_point const& tp);
    void pingPeer();
    void maybeProcessPingResponse(Hash const& id);
    VirtualClock::time_point mPingSentTime;
    std::chrono::milliseconds mLastPing;

    PeerMetrics mPeerMetrics;
    ReadingCapacity mCapacity;

    OverlayMetrics& getOverlayMetrics();

    bool shouldAbort() const;
    void recvRawMessage(StellarMessage const& msg);
    void recvMessage(StellarMessage const& msg);
    void recvMessage(AuthenticatedMessage const& msg);
    void recvMessage(xdr::msg_ptr const& xdrBytes);

    virtual void recvError(StellarMessage const& msg);
    void updatePeerRecordAfterEcho();
    void updatePeerRecordAfterAuthentication();
    void recvAuth(StellarMessage const& msg);
    void recvDontHave(StellarMessage const& msg);
    void recvGetPeers(StellarMessage const& msg);
    void recvHello(Hello const& elo);
    void recvPeers(StellarMessage const& msg);
    void recvSurveyRequestMessage(StellarMessage const& msg);
    void recvSurveyResponseMessage(StellarMessage const& msg);
    void recvSendMore(StellarMessage const& msg);

    void recvGetTxSet(StellarMessage const& msg);
    void recvTxSet(StellarMessage const& msg);
    void recvGeneralizedTxSet(StellarMessage const& msg);
    void recvTransaction(StellarMessage const& msg);
    void recvGetSCPQuorumSet(StellarMessage const& msg);
    void recvSCPQuorumSet(StellarMessage const& msg);
    void recvSCPMessage(StellarMessage const& msg);
    void recvGetSCPState(StellarMessage const& msg);
    void recvFloodAdvert(StellarMessage const& msg);
    void recvFloodDemand(StellarMessage const& msg);

    void sendHello();
    void sendAuth();
    void sendSCPQuorumSet(SCPQuorumSetPtr qSet);
    void sendDontHave(MessageType type, uint256 const& itemID);
    void sendPeers();
    void sendError(ErrorCode error, std::string const& message);
    void sendSendMore(uint32_t numMessages);

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
    virtual bool
    sendQueueIsOverloaded() const
    {
        return false;
    }

    virtual AuthCert getAuthCert();

    void startRecurrentTimer();
    void recurrentTimerExpired(asio::error_code const& error);
    std::chrono::seconds getIOTimeout() const;

    // helper method to acknownledge that some bytes were received
    void receivedBytes(size_t byteCount, bool gotFullMessage);

    void sendAuthenticatedMessage(StellarMessage const& msg);

    void beginMesssageProcessing(StellarMessage const& msg);
    void endMessageProcessing(StellarMessage const& msg);

    void maybeSendNextBatch();

    TxAdvertQueue mTxAdvertQueue;

    // How many _hashes_ in total are queued?
    // NB: Each advert & demand contains a _vector_ of tx hashes.
    size_t mAdvertQueueTxHashCount{0};
    size_t mDemandQueueTxHashCount{0};

    // As of MIN_OVERLAY_VERSION_FOR_FLOOD_ADVERT, peers accumulate an _advert_
    // of flood messages, then periodically flush the advert and await a
    // _demand_ message with a list of flood messages to send. Adverts are
    // typically smaller than full messages and batching them means we also
    // amortize the authentication framing.
    TxAdvertVector mTxHashesToAdvertise;
    void flushAdvert();
    VirtualTimer mAdvertTimer;
    void startAdvertTimer();
    // transaction hash -> ledger number
    RandomEvictionCache<Hash, uint32_t> mAdvertHistory;

    bool mShuttingDown{false};

#ifdef BUILD_TESTS
    // For testing purposes, sometimes we want to force
    // to disable the pull mode flag in the auth
    // message. Set this optional bool value to force it.
    // This will be obsolete once the min overlay version
    // becomes >= FIRST_VERSION_REQUIRING_PULL_MODE.
    bool mOverrideDisablePullModeForTesting{false};
#endif

  public:
    Peer(Application& app, PeerRole role);

    Application&
    getApp()
    {
        return mApp;
    }

    void shutdown();
    void clearBelow(uint32_t ledgerSeq);

    std::string msgSummary(StellarMessage const& stellarMsg);
    void sendGetTxSet(uint256 const& setID);
    void sendGetQuorumSet(uint256 const& setID);
    void sendGetPeers();
    void sendGetScpState(uint32 ledgerSeq);
    void sendErrorAndDrop(ErrorCode error, std::string const& message,
                          DropMode dropMode);

    void sendMessage(std::shared_ptr<StellarMessage const> msg,
                     bool log = true);

    PeerRole
    getRole() const
    {
        return mRole;
    }

    bool isConnected() const;
    bool isAuthenticated() const;

    VirtualClock::time_point
    getCreationTime() const
    {
        return mCreationTime;
    }
    std::chrono::seconds getLifeTime() const;
    std::chrono::milliseconds getPing() const;

    PeerState
    getState() const
    {
        return mState;
    }

    std::string const&
    getRemoteVersion() const
    {
        return mRemoteVersion;
    }

    uint32_t
    getRemoteOverlayMinVersion() const
    {
        return mRemoteOverlayMinVersion;
    }

    uint32_t
    getRemoteOverlayVersion() const
    {
        return mRemoteOverlayVersion;
    }

    PeerBareAddress const&
    getAddress()
    {
        return mAddress;
    }

    NodeID
    getPeerID()
    {
        return mPeerID;
    }

    PeerMetrics&
    getPeerMetrics()
    {
        return mPeerMetrics;
    }

    std::string const& toString();
    virtual std::string getIP() const = 0;

    // These exist mostly to be overridden in TCPPeer and callable via
    // shared_ptr<Peer> as a captured shared_from_this().
    virtual void connectHandler(asio::error_code const& ec);

    virtual void
    writeHandler(asio::error_code const& error, size_t bytes_transferred,
                 size_t messages_transferred)
    {
    }

    virtual void
    readHeaderHandler(asio::error_code const& error, size_t bytes_transferred)
    {
    }

    virtual void
    readBodyHandler(asio::error_code const& error, size_t bytes_transferred,
                    size_t expected_length)
    {
    }

    virtual void drop(std::string const& reason, DropDirection dropDirection,
                      DropMode dropMode) = 0;
    virtual ~Peer()
    {
    }

    void sendTxDemand(TxDemandVector&& demands);
    void fulfillDemand(FloodDemand const& dmd);
    void queueTxHashToAdvertise(Hash const& hash);
    void queueTxHashAndMaybeTrim(Hash const& hash);
    TxAdvertQueue&
    getTxAdvertQueue()
    {
        return mTxAdvertQueue;
    };

    friend class LoopbackPeer;
};
}
