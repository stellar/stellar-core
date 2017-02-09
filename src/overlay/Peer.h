#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "database/Database.h"
#include "overlay/StellarXDR.h"
#include "util/NonCopyable.h"
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

/*
 * Another peer out there that we are connected to
 */
class Peer : public std::enable_shared_from_this<Peer>,
             public NonMovableOrCopyable
{

  public:
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

    static medida::Meter& getByteReadMeter(Application& app);
    static medida::Meter& getByteWriteMeter(Application& app);

  protected:
    Application& mApp;

    PeerRole mRole;
    PeerState mState;
    NodeID mPeerID;
    uint256 mSendNonce;
    uint256 mRecvNonce;

    HmacSha256Key mSendMacKey;
    HmacSha256Key mRecvMacKey;
    uint64_t mSendMacSeq{0};
    uint64_t mRecvMacSeq{0};

    std::string mRemoteVersion;
    uint32_t mRemoteOverlayMinVersion;
    uint32_t mRemoteOverlayVersion;
    unsigned short mRemoteListeningPort;

    VirtualTimer mIdleTimer;
    VirtualClock::time_point mLastRead;
    VirtualClock::time_point mLastWrite;

    medida::Meter& mMessageRead;
    medida::Meter& mMessageWrite;
    medida::Meter& mByteRead;
    medida::Meter& mByteWrite;
    medida::Meter& mErrorRead;
    medida::Meter& mErrorWrite;
    medida::Meter& mTimeoutIdle;

    medida::Timer& mRecvErrorTimer;
    medida::Timer& mRecvHelloTimer;
    medida::Timer& mRecvAuthTimer;
    medida::Timer& mRecvDontHaveTimer;
    medida::Timer& mRecvGetPeersTimer;
    medida::Timer& mRecvPeersTimer;
    medida::Timer& mRecvGetTxSetTimer;
    medida::Timer& mRecvTxSetTimer;
    medida::Timer& mRecvTransactionTimer;
    medida::Timer& mRecvGetSCPQuorumSetTimer;
    medida::Timer& mRecvSCPQuorumSetTimer;
    medida::Timer& mRecvSCPMessageTimer;
    medida::Timer& mRecvGetSCPStateTimer;

    medida::Timer& mRecvSCPPrepareTimer;
    medida::Timer& mRecvSCPConfirmTimer;
    medida::Timer& mRecvSCPNominateTimer;
    medida::Timer& mRecvSCPExternalizeTimer;

    medida::Meter& mSendErrorMeter;
    medida::Meter& mSendHelloMeter;
    medida::Meter& mSendAuthMeter;
    medida::Meter& mSendDontHaveMeter;
    medida::Meter& mSendGetPeersMeter;
    medida::Meter& mSendPeersMeter;
    medida::Meter& mSendGetTxSetMeter;
    medida::Meter& mSendTransactionMeter;
    medida::Meter& mSendTxSetMeter;
    medida::Meter& mSendGetSCPQuorumSetMeter;
    medida::Meter& mSendSCPQuorumSetMeter;
    medida::Meter& mSendSCPMessageSetMeter;
    medida::Meter& mSendGetSCPStateMeter;

    medida::Meter& mDropInConnectHandlerMeter;
    medida::Meter& mDropInRecvMessageDecodeMeter;
    medida::Meter& mDropInRecvMessageSeqMeter;
    medida::Meter& mDropInRecvMessageMacMeter;
    medida::Meter& mDropInRecvMessageUnauthMeter;
    medida::Meter& mDropInRecvHelloUnexpectedMeter;
    medida::Meter& mDropInRecvHelloVersionMeter;
    medida::Meter& mDropInRecvHelloSelfMeter;
    medida::Meter& mDropInRecvHelloPeerIDMeter;
    medida::Meter& mDropInRecvHelloCertMeter;
    medida::Meter& mDropInRecvHelloBanMeter;
    medida::Meter& mDropInRecvHelloNetMeter;
    medida::Meter& mDropInRecvHelloPortMeter;
    medida::Meter& mDropInRecvAuthUnexpectedMeter;
    medida::Meter& mDropInRecvAuthRejectMeter;
    medida::Meter& mDropInRecvAuthInvalidPeerMeter;
    medida::Meter& mDropInRecvErrorMeter;

    bool shouldAbort() const;
    void recvMessage(StellarMessage const& msg);
    void recvMessage(AuthenticatedMessage const& msg);
    void recvMessage(xdr::msg_ptr const& xdrBytes);

    virtual void recvError(StellarMessage const& msg);
    // returns false if we should drop this peer
    void noteHandshakeSuccessInPeerRecord();
    void recvAuth(StellarMessage const& msg);
    void recvDontHave(StellarMessage const& msg);
    void recvGetPeers(StellarMessage const& msg);
    void recvHello(Hello const& elo);
    void recvPeers(StellarMessage const& msg);

    void recvGetTxSet(StellarMessage const& msg);
    void recvTxSet(StellarMessage const& msg);
    void recvTransaction(StellarMessage const& msg);
    void recvGetSCPQuorumSet(StellarMessage const& msg);
    void recvSCPQuorumSet(StellarMessage const& msg);
    void recvSCPMessage(StellarMessage const& msg);
    void recvGetSCPState(StellarMessage const& msg);

    void sendHello();
    void sendAuth();
    void sendSCPQuorumSet(SCPQuorumSetPtr qSet);
    void sendDontHave(MessageType type, uint256 const& itemID);
    void sendPeers();

    // NB: This is a move-argument because the write-buffer has to travel
    // with the write-request through the async IO system, and we might have
    // several queued at once. We have carefully arranged this to not copy
    // data more than the once necessary into this buffer, but it can't be
    // put in a reused/non-owned buffer without having to buffer/queue
    // messages somewhere else. The async write request will point _into_
    // this owned buffer. This is really the best we can do.
    virtual void sendMessage(xdr::msg_ptr&& xdrBytes) = 0;
    virtual void
    connected()
    {
    }

    virtual AuthCert getAuthCert();

    void startIdleTimer();
    void idleTimerExpired(asio::error_code const& error);
    size_t getIOTimeoutSeconds() const;

    // helper method to acknownledge that some bytes were received
    void receivedBytes(size_t byteCount, bool gotFullMessage);

  public:
    Peer(Application& app, PeerRole role);

    Application&
    getApp()
    {
        return mApp;
    }

    void sendGetTxSet(uint256 const& setID);
    void sendGetQuorumSet(uint256 const& setID);
    void sendGetPeers();
    void sendGetScpState(uint32 ledgerSeq);

    void sendMessage(StellarMessage const& msg);

    PeerRole
    getRole() const
    {
        return mRole;
    }

    bool isConnected() const;
    bool isAuthenticated() const;

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

    unsigned short
    getRemoteListeningPort()
    {
        return mRemoteListeningPort;
    }
    NodeID
    getPeerID()
    {
        return mPeerID;
    }

    std::string toString();

    // These exist mostly to be overridden in TCPPeer and callable via
    // shared_ptr<Peer> as a captured shared_from_this().
    virtual void connectHandler(asio::error_code const& ec);

    virtual void
    writeHandler(asio::error_code const& error, size_t bytes_transferred)
    {
    }

    virtual void
    readHeaderHandler(asio::error_code const& error, size_t bytes_transferred)
    {
    }

    virtual void
    readBodyHandler(asio::error_code const& error, size_t bytes_transferred)
    {
    }

    void drop(ErrorCode err, std::string const& msg);
    virtual void drop() = 0;
    virtual std::string getIP() = 0;
    virtual ~Peer()
    {
    }

    friend class LoopbackPeer;
};
}
