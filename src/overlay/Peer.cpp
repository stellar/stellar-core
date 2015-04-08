// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Peer.h"

#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "generated/StellarXDR.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerRecord.h"
#include "util/Logging.h"

#include "xdrpp/marshal.h"

#include <soci.h>
#include <time.h>

// LATER: need to add some way of docking peers that are misbehaving by sending
// you bad data

namespace stellar
{

using namespace std;
using namespace soci;

Peer::Peer(Application& app, PeerRole role)
    : mApp(app)
    , mRole(role)
    , mState(role == ACCEPTOR ? CONNECTING : CONNECTED)
    , mRemoteProtocolVersion(0)
    , mRemoteListeningPort(0)
{
}

void
Peer::sendHello()
{
    LOG(DEBUG) << "Peer::sendHello "
               << "@" << mApp.getConfig().PEER_PORT << " to "
               << mRemoteListeningPort;

    StellarMessage msg;
    msg.type(HELLO);
    msg.hello().protocolVersion = mApp.getConfig().PROTOCOL_VERSION;
    msg.hello().versionStr = mApp.getConfig().VERSION_STR;
    msg.hello().listeningPort = mApp.getConfig().PEER_PORT;
    msg.hello().peerID = mApp.getConfig().PEER_PUBLIC_KEY;

    sendMessage(msg);
}

std::string
Peer::toString()
{
    std::stringstream s;
    s << getIP() << ":" << mRemoteListeningPort;
    return s.str();
}

void
Peer::connectHandler(asio::error_code const& error)
{
    if (error)
    {
        CLOG(WARNING, "Overlay")
            << "@" << mApp.getConfig().PEER_PORT
            << " connectHandler error: " << error.message();
        drop();
    }
    else
    {
        CLOG(DEBUG, "Overlay") << "connected @" << toString();
        connected();
        mState = CONNECTED;
        sendHello();
    }
}

void
Peer::sendDontHave(MessageType type, uint256 const& itemID)
{
    StellarMessage msg;
    msg.type(DONT_HAVE);
    msg.dontHave().reqHash = itemID;
    msg.dontHave().type = type;

    sendMessage(msg);
}

void
Peer::sendSCPQuorumSet(SCPQuorumSetPtr qSet)
{
    StellarMessage msg;
    msg.type(SCP_QUORUMSET);
    msg.qSet() = *qSet;

    sendMessage(msg);
}
void
Peer::sendGetTxSet(uint256 const& setID)
{
    StellarMessage newMsg;
    newMsg.type(GET_TX_SET);
    newMsg.txSetHash() = setID;

    sendMessage(newMsg);
}
void
Peer::sendGetQuorumSet(uint256 const& setID)
{
    CLOG(TRACE, "Overlay") << "Get quorum set: " << hexAbbrev(setID);

    StellarMessage newMsg;
    newMsg.type(GET_SCP_QUORUMSET);
    newMsg.qSetHash() = setID;

    sendMessage(newMsg);
}

void
Peer::sendPeers()
{
    // send top 50 peers we know about
    vector<PeerRecord> peerList;
    PeerRecord::loadPeerRecords(mApp.getDatabase(), 50, mApp.getClock().now(),
                                peerList);
    StellarMessage newMsg;
    newMsg.type(PEERS);
    newMsg.peers().resize(xdr::size32(peerList.size()));
    for (size_t n = 0; n < peerList.size(); n++)
    {
        if (!peerList[n].isPrivateAddress())
        {
            peerList[n].toXdr(newMsg.peers()[n]);
        }
    }
    sendMessage(newMsg);
}

void
Peer::sendMessage(StellarMessage const& msg)
{
    CLOG(TRACE, "Overlay") << "("
                           << binToHex(mApp.getConfig().PEER_PUBLIC_KEY)
                                  .substr(0, 6) << ")send: " << msg.type()
                           << " to : " << hexAbbrev(mPeerID);
    xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(msg));
    this->sendMessage(std::move(xdrBytes));
}

void
Peer::recvMessage(xdr::msg_ptr const& msg)
{
    CLOG(TRACE, "Overlay") << "received xdr::msg_ptr";
    StellarMessage sm;
    xdr::xdr_from_msg(msg, sm);
    recvMessage(sm);
}

void
Peer::recvMessage(StellarMessage const& stellarMsg)
{
    CLOG(TRACE, "Overlay") << "("
                           << binToHex(mApp.getConfig().PEER_PUBLIC_KEY)
                                  .substr(0, 6)
                           << ")recv: " << stellarMsg.type()
                           << " from:" << hexAbbrev(mPeerID);

    if (mState < GOT_HELLO &&
        ((stellarMsg.type() != HELLO) && (stellarMsg.type() != PEERS)))
    {
        CLOG(WARNING, "Overlay") << "recv: " << stellarMsg.type()
                                 << " before hello";
        drop();
        return;
    }

    switch (stellarMsg.type())
    {
    case ERROR_MSG:
    {
        recvError(stellarMsg);
    }
    break;

    case HELLO:
    {
        this->recvHello(stellarMsg);
    }
    break;

    case DONT_HAVE:
    {
        recvDontHave(stellarMsg);
    }
    break;

    case GET_PEERS:
    {
        recvGetPeers(stellarMsg);
    }
    break;

    case PEERS:
    {
        recvPeers(stellarMsg);
    }
    break;

    case GET_TX_SET:
    {
        recvGetTxSet(stellarMsg);
    }
    break;

    case TX_SET:
    {
        recvTxSet(stellarMsg);
    }
    break;

    case TRANSACTION:
    {
        recvTransaction(stellarMsg);
    }
    break;

    case GET_SCP_QUORUMSET:
    {
        recvGetSCPQuorumSet(stellarMsg);
    }
    break;

    case SCP_QUORUMSET:
    {
        recvSCPQuorumSet(stellarMsg);
    }
    break;

    case SCP_MESSAGE:
    {
        recvSCPMessage(stellarMsg);
    }
    break;
    }
}

void
Peer::recvDontHave(StellarMessage const& msg)
{
    switch (msg.dontHave().type)
    {
    case TX_SET:
        mApp.getHerder().doesntHaveTxSet(msg.dontHave().reqHash,
                                         shared_from_this());
        break;
    case SCP_QUORUMSET:
        mApp.getHerder().doesntHaveSCPQuorumSet(msg.dontHave().reqHash,
                                                shared_from_this());
        break;
    default:
        break;
    }
}

void
Peer::recvGetTxSet(StellarMessage const& msg)
{
    TxSetFramePtr txSet = mApp.getHerder().fetchTxSet(msg.txSetHash(), false);
    if (txSet)
    {
        StellarMessage newMsg;
        newMsg.type(TX_SET);
        txSet->toXDR(newMsg.txSet());

        sendMessage(newMsg);
    }
    else
    {
        sendDontHave(TX_SET, msg.txSetHash());
    }
}
void
Peer::recvTxSet(StellarMessage const& msg)
{
    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(msg.txSet());
    mApp.getHerder().recvTxSet(txSet);
}

void
Peer::recvTransaction(StellarMessage const& msg)
{
    TransactionFramePtr transaction =
        TransactionFrame::makeTransactionFromWire(msg.transaction());
    if (transaction)
    {
        // add it to our current set
        // and make sure it is valid
        if (mApp.getHerder().recvTransaction(transaction))
        {
            mApp.getOverlayManager().recvFloodedMsg(msg, shared_from_this());
            mApp.getOverlayManager().broadcastMessage(msg);
        }
    }
}

void
Peer::recvGetSCPQuorumSet(StellarMessage const& msg)
{
    SCPQuorumSetPtr qSet =
        mApp.getHerder().fetchSCPQuorumSet(msg.qSetHash(), false);
    if (qSet)
    {
        sendSCPQuorumSet(qSet);
    }
    else
    {
        CLOG(TRACE, "Overlay")
            << "No quorum set: " << hexAbbrev(msg.qSetHash());
        sendDontHave(SCP_QUORUMSET, msg.qSetHash());
        // do we want to ask other people for it?
    }
}
void
Peer::recvSCPQuorumSet(StellarMessage const& msg)
{
    SCPQuorumSetPtr qSet = std::make_shared<SCPQuorumSet>(msg.qSet());
    mApp.getHerder().recvSCPQuorumSet(qSet);
}

void
Peer::recvSCPMessage(StellarMessage const& msg)
{
    SCPEnvelope envelope = msg.envelope();
    CLOG(TRACE, "Overlay") << "recvSCPMessage qset: "
                           << binToHex(msg.envelope().statement.quorumSetHash)
                                  .substr(0, 6);

    mApp.getOverlayManager().recvFloodedMsg(msg, shared_from_this());

    auto cb = [msg, this](SCP::EnvelopeState state)
    {
        if (state == SCP::EnvelopeState::VALID)
        {
            mApp.getOverlayManager().broadcastMessage(msg);
        }
    };
    mApp.getHerder().recvSCPEnvelope(envelope, cb);
}

void
Peer::recvError(StellarMessage const& msg)
{
    // TODO.4
}

bool
Peer::recvHello(StellarMessage const& msg)
{
    if (msg.hello().peerID == mApp.getConfig().PEER_PUBLIC_KEY)
    {
        CLOG(DEBUG, "Overlay") << "connecting to self";
        drop();
        return false;
    }

    mRemoteProtocolVersion = msg.hello().protocolVersion;
    mRemoteVersion = msg.hello().versionStr;
    if (msg.hello().listeningPort <= 0 ||
        msg.hello().listeningPort > UINT16_MAX)
    {
        CLOG(DEBUG, "Overlay") << "bad port in recvHello";
        drop();
        return false;
    }
    mRemoteListeningPort =
        static_cast<unsigned short>(msg.hello().listeningPort);
    CLOG(INFO, "Overlay") << "recvHello "
                          << "@" << mApp.getConfig().PEER_PORT
                          << " from: " << mRemoteProtocolVersion << " "
                          << mRemoteVersion << " " << mRemoteListeningPort;
    mState = GOT_HELLO;
    mPeerID = msg.hello().peerID;
    return true;
}

void
Peer::recvGetPeers(StellarMessage const& msg)
{
    sendPeers();
}

void
Peer::recvPeers(StellarMessage const& msg)
{
    for (auto peer : msg.peers())
    {
        stringstream ip;

        ip << (int)peer.ip[0] << "." << (int)peer.ip[1] << "."
           << (int)peer.ip[2] << "." << (int)peer.ip[3];

        if (peer.port == 0 || peer.port > UINT16_MAX)
        {
            CLOG(DEBUG, "Overlay") << "ignoring peer with bad port";
            continue;
        }
        PeerRecord pr{ip.str(), static_cast<unsigned short>(peer.port),
                      mApp.getClock().now(), peer.numFailures, 1};

        if (pr.isPrivateAddress())
        {
            CLOG(DEBUG, "Overlay") << "ignoring flooded private address";
        }
        else
        {
            pr.insertIfNew(mApp.getDatabase());
        }
    }
}
}
