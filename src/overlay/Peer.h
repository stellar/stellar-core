#ifndef __PEER__
#define __PEER__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>
#include "xdrpp/message.h"
#include "generated/StellarXDR.h"
#include "fba/QuorumSet.h"

/*
Another peer out there that we are connected to
*/

namespace stellar
{
using namespace std;

class Application;
class LoopbackPeer;

class Peer : public enable_shared_from_this<Peer>
{

  public:
    typedef std::shared_ptr<Peer> pointer;

    enum PeerState
    {
        CONNECTING,
        CONNECTED,
        GOT_HELLO,
        CLOSING
    };

    enum PeerRole
    {
        INITIATOR,
        ACCEPTOR
    };

  protected:
    Application& mApp;

    PeerRole mRole;
    PeerState mState;

    std::string mRemoteVersion;
    int mRemoteProtocolVersion;
    int mRemoteListeningPort;
    void recvMessage(stellarxdr::StellarMessage const& msg);
    void recvMessage(xdr::msg_ptr const& xdrBytes);

    virtual void recvError(stellarxdr::StellarMessage const& msg);
    virtual void recvHello(stellarxdr::StellarMessage const& msg);
    void recvDontHave(stellarxdr::StellarMessage const& msg);
    void recvGetPeers(stellarxdr::StellarMessage const& msg);
    void recvPeers(stellarxdr::StellarMessage const& msg);
    void recvGetHistory(stellarxdr::StellarMessage const& msg);
    void recvHistory(stellarxdr::StellarMessage const& msg);
    void recvGetDelta(stellarxdr::StellarMessage const& msg);
    void recvDelta(stellarxdr::StellarMessage const& msg);
    void recvGetTxSet(stellarxdr::StellarMessage const& msg);
    void recvTxSet(stellarxdr::StellarMessage const& msg);
    void recvGetValidations(stellarxdr::StellarMessage const& msg);
    void recvValidations(stellarxdr::StellarMessage const& msg);
    void recvTransaction(stellarxdr::StellarMessage const& msg);
    void recvGetQuorumSet(stellarxdr::StellarMessage const& msg);
    void recvQuorumSet(stellarxdr::StellarMessage const& msg);
    void recvFBAMessage(stellarxdr::StellarMessage const& msg);

    void sendHello();
    void sendQuorumSet(QuorumSet::pointer qSet);
    void sendDontHave(stellarxdr::MessageType type,
                      stellarxdr::uint256 const& itemID);
    void sendPeers();

    // NB: This is a move-argument because the write-buffer has to travel
    // with the write-request through the async IO system, and we might have
    // several queued at once. We have carefully arranged this to not copy
    // data more than the once necessary into this buffer, but it can't be
    // put in a reused/non-owned buffer without having to buffer/queue
    // messages somewhere else. The async write request will point _into_
    // this owned buffer. This is really the best we can do.
    virtual void sendMessage(xdr::msg_ptr&& xdrBytes) = 0;

  public:
    Peer(Application& app, PeerRole role);
    Application&
    getApp()
    {
        return mApp;
    }

    void sendGetTxSet(stellarxdr::uint256 const& setID);
    void sendGetQuorumSet(stellarxdr::uint256 const& setID);

    void sendMessage(stellarxdr::StellarMessage const& msg);

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
    getRemoreVersion() const
    {
        return mRemoteVersion;
    }
    int
    getRemoteProtocolVersion() const
    {
        return mRemoteProtocolVersion;
    }
    int
    getRemoteListeningPort()
    {
        return mRemoteListeningPort;
    }

    // These exist mostly to be overridden in TCPPeer and callable via
    // shared_ptr<Peer> as a captured shared_from_this().
    virtual void connectHandler(const asio::error_code& ec);
    virtual void
    writeHandler(const asio::error_code& error, std::size_t bytes_transferred)
    {
    }
    virtual void
    readHeaderHandler(const asio::error_code& error,
                      std::size_t bytes_transferred)
    {
    }
    virtual void
    readBodyHandler(const asio::error_code& error,
                    std::size_t bytes_transferred)
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

#endif
