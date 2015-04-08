#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "xdrpp/message.h"
#include "generated/StellarXDR.h"
#include "generated/SCPXDR.h"
#include "util/Timer.h"
#include "database/Database.h"

namespace stellar
{

typedef std::shared_ptr<SCPQuorumSet> SCPQuorumSetPtr;

class Application;
class LoopbackPeer;

/*
 * Another peer out there that we are connected to
 */
class Peer : public std::enable_shared_from_this<Peer>
{

  public:
    typedef std::shared_ptr<Peer> pointer;

    enum PeerState
    {
        CONNECTING = 0,
        CONNECTED = 1,
        GOT_HELLO = 2,
        CLOSING = 3
    };

    enum PeerRole
    {
        INITIATOR,
        ACCEPTOR
    };

  protected:
    Application& mApp;

    PeerRole mRole; // from point of view of the other end
    PeerState mState;
    uint256 mPeerID;

    std::string mRemoteVersion;
    uint32_t mRemoteProtocolVersion;
    unsigned short mRemoteListeningPort;
    void recvMessage(StellarMessage const& msg);
    void recvMessage(xdr::msg_ptr const& xdrBytes);

    virtual void recvError(StellarMessage const& msg);
    // returns false if we should drop this peer
    virtual bool recvHello(StellarMessage const& msg);
    void recvDontHave(StellarMessage const& msg);
    void recvGetPeers(StellarMessage const& msg);
    void recvPeers(StellarMessage const& msg);
    void recvGetHistory(StellarMessage const& msg);
    void recvHistory(StellarMessage const& msg);
    void recvGetDelta(StellarMessage const& msg);
    void recvDelta(StellarMessage const& msg);
    void recvGetTxSet(StellarMessage const& msg);
    void recvTxSet(StellarMessage const& msg);
    void recvTransaction(StellarMessage const& msg);
    void recvGetSCPQuorumSet(StellarMessage const& msg);
    void recvSCPQuorumSet(StellarMessage const& msg);
    void recvSCPMessage(StellarMessage const& msg);

    void sendHello();
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

  public:
    Peer(Application& app, PeerRole role);

    Application&
    getApp()
    {
        return mApp;
    }

    void sendGetTxSet(uint256 const& setID);
    void sendGetQuorumSet(uint256 const& setID);

    void sendMessage(StellarMessage const& msg);

    PeerRole
    getRole() const
    {
        return mRole;
    }

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
    getRemoteProtocolVersion() const
    {
        return mRemoteProtocolVersion;
    }

    unsigned short
    getRemoteListeningPort()
    {
        return mRemoteListeningPort;
    }
    uint256
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

    virtual void drop() = 0;
    virtual std::string getIP() = 0;
    virtual ~Peer()
    {
    }

    friend class LoopbackPeer;
};
}
