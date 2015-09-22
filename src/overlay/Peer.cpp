// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Peer.h"

#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/Random.h"
#include "database/Database.h"
#include "overlay/StellarXDR.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerAuth.h"
#include "overlay/PeerRecord.h"
#include "util/Logging.h"

#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "medida/meter.h"

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
    , mState(role == WE_CALLED_REMOTE ? CONNECTING : CONNECTED)
    , mRemoteOverlayVersion(0)
    , mRemoteListeningPort(0)
    , mRecvErrorTimer(app.getMetrics().NewTimer({"overlay", "recv", "error"}))
    , mRecvHelloTimer(app.getMetrics().NewTimer({"overlay", "recv", "hello"}))
    , mRecvAuthTimer(app.getMetrics().NewTimer({"overlay", "recv", "auth"}))
    , mRecvDontHaveTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "dont-have"}))
    , mRecvGetPeersTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-peers"}))
    , mRecvPeersTimer(app.getMetrics().NewTimer({"overlay", "recv", "peers"}))
    , mRecvGetTxSetTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-txset"}))
    , mRecvTxSetTimer(app.getMetrics().NewTimer({"overlay", "recv", "txset"}))
    , mRecvTransactionTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "transaction"}))
    , mRecvGetSCPQuorumSetTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-scp-qset"}))
    , mRecvSCPQuorumSetTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-qset"}))
    , mRecvSCPMessageTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-message"}))

    , mSendErrorMeter(
        app.getMetrics().NewMeter({"overlay", "send", "error"},
                                  "message"))
    , mSendHelloMeter(
        app.getMetrics().NewMeter({"overlay", "send", "hello"},
                                  "message"))
    , mSendAuthMeter(
        app.getMetrics().NewMeter({"overlay", "send", "auth"},
                                  "message"))
    , mSendDontHaveMeter(
        app.getMetrics().NewMeter({"overlay", "send", "dont-have"},
                                  "message"))
    , mSendGetPeersMeter(
        app.getMetrics().NewMeter({"overlay", "send", "get-peers"},
                                  "message"))
    , mSendPeersMeter(
        app.getMetrics().NewMeter({"overlay", "send", "peers"},
                                  "message"))
    , mSendGetTxSetMeter(
        app.getMetrics().NewMeter({"overlay", "send", "get-txset"},
                                  "message"))
    , mSendTxSetMeter(
        app.getMetrics().NewMeter({"overlay", "send", "txset"},
                                  "message"))
    , mSendTransactionMeter(
        app.getMetrics().NewMeter({"overlay", "send", "transaction"},
                                  "message"))
    , mSendGetSCPQuorumSetMeter(
        app.getMetrics().NewMeter({"overlay", "send", "get-scp-qset"},
                                  "message"))
    , mSendSCPQuorumSetMeter(
        app.getMetrics().NewMeter({"overlay", "send", "scp-qset"},
                                  "message"))

    , mDropInConnectHandlerMeter(
        app.getMetrics().NewMeter({"overlay", "drop", "connect-handler"},
                                  "drop"))
    , mDropInRecvMessageDecodeMeter(
        app.getMetrics().NewMeter({"overlay", "drop", "recv-message-decode"},
                                  "drop"))
    , mDropInRecvMessageSeqMeter(
        app.getMetrics().NewMeter({"overlay", "drop", "recv-message-seq"},
                                  "drop"))
    , mDropInRecvMessageMacMeter(
        app.getMetrics().NewMeter({"overlay", "drop", "recv-message-mac"},
                                  "drop"))
    , mDropInRecvMessageUnauthMeter(
        app.getMetrics().NewMeter({"overlay", "drop", "recv-message-unauth"},
                                  "drop"))
    , mDropInRecvHelloVersionMeter(
        app.getMetrics().NewMeter({"overlay", "drop", "recv-hello-version"},
                                  "drop"))
    , mDropInRecvHelloSelfMeter(
        app.getMetrics().NewMeter({"overlay", "drop", "recv-hello-self"},
                                  "drop"))
    , mDropInRecvHelloCertMeter(
        app.getMetrics().NewMeter({"overlay", "drop", "recv-hello-cert"},
                                  "drop"))
    , mDropInRecvHelloNetMeter(
        app.getMetrics().NewMeter({"overlay", "drop", "recv-hello-net"},
                                  "drop"))
    , mDropInRecvHelloPortMeter(
        app.getMetrics().NewMeter({"overlay", "drop", "recv-hello-port"},
                                  "drop"))
    , mDropInRecvAuthUnexpectedMeter(
        app.getMetrics().NewMeter({"overlay", "drop", "recv-auth-unexpected"},
                                  "drop"))
    , mDropInRecvAuthRejectMeter(
        app.getMetrics().NewMeter({"overlay", "drop", "recv-auth-reject"},
                                  "drop"))

{
    auto bytes = randomBytes(mSendNonce.size());
    std::copy(bytes.begin(), bytes.end(), mSendNonce.begin());
}

void
Peer::sendHello()
{
    CLOG(DEBUG, "Overlay") << "Peer::sendHello to " << toString();
    StellarMessage msg;
    msg.type(HELLO);
    msg.hello().ledgerVersion = mApp.getConfig().LEDGER_PROTOCOL_VERSION;
    msg.hello().overlayVersion = mApp.getConfig().OVERLAY_PROTOCOL_VERSION;
    msg.hello().versionStr = mApp.getConfig().VERSION_STR;
    msg.hello().networkID = mApp.getNetworkID();
    msg.hello().listeningPort = mApp.getConfig().PEER_PORT;
    msg.hello().peerID = mApp.getConfig().NODE_SEED.getPublicKey();
    msg.hello().cert = this->getAuthCert();
    msg.hello().nonce = mSendNonce;
    sendMessage(msg);
    mSendHelloMeter.Mark();
}

AuthCert
Peer::getAuthCert()
{
    return mApp.getOverlayManager().getPeerAuth().getAuthCert();
}

void
Peer::sendAuth()
{
    StellarMessage msg;
    msg.type(AUTH);
    sendMessage(msg);
    mSendAuthMeter.Mark();
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
            << " connectHandler error: " << error.message();
        mDropInConnectHandlerMeter.Mark();
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
    mSendDontHaveMeter.Mark();
}

void
Peer::sendSCPQuorumSet(SCPQuorumSetPtr qSet)
{
    StellarMessage msg;
    msg.type(SCP_QUORUMSET);
    msg.qSet() = *qSet;

    sendMessage(msg);
    mSendSCPQuorumSetMeter.Mark();
}
void
Peer::sendGetTxSet(uint256 const& setID)
{
    StellarMessage newMsg;
    newMsg.type(GET_TX_SET);
    newMsg.txSetHash() = setID;

    sendMessage(newMsg);
    mSendGetTxSetMeter.Mark();
}
void
Peer::sendGetQuorumSet(uint256 const& setID)
{
    CLOG(TRACE, "Overlay") << "Get quorum set: " << hexAbbrev(setID);

    StellarMessage newMsg;
    newMsg.type(GET_SCP_QUORUMSET);
    newMsg.qSetHash() = setID;

    sendMessage(newMsg);
    mSendGetSCPQuorumSetMeter.Mark();
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
    newMsg.peers().reserve(peerList.size());
    for (auto const& pr : peerList)
    {
        if (pr.isPrivateAddress())
        {
            continue;
        }
        PeerAddress pa;
        pr.toXdr(pa);
        newMsg.peers().push_back(pa);
    }
    sendMessage(newMsg);
    mSendPeersMeter.Mark();
}

void
Peer::sendMessage(StellarMessage const& msg)
{
    CLOG(TRACE, "Overlay") << "("
                           << PubKeyUtils::toShortString(
                               mApp.getConfig().NODE_SEED.getPublicKey())
                           << ") send: " << msg.type()
                           << " to : " << PubKeyUtils::toShortString(mPeerID);

    if (msg.type() == HELLO)
    {
        assert((mState == CONNECTED && mRole == WE_CALLED_REMOTE) ||
               (mState == GOT_HELLO && mRole == REMOTE_CALLED_US));
        xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(msg));
        this->sendMessage(std::move(xdrBytes));
    }
    else
    {
        AuthenticatedMessage amsg;
        amsg.message = msg;
        amsg.sequence = mSendMacSeq;
        amsg.mac = hmacSha256(mSendMacKey,
                              xdr::xdr_to_opaque(mSendMacSeq, msg));
        ++mSendMacSeq;
        xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(amsg));
        this->sendMessage(std::move(xdrBytes));
    }
}

void
Peer::recvMessage(xdr::msg_ptr const& msg)
{
    CLOG(TRACE, "Overlay") << "received xdr::msg_ptr";
    try
    {
        if (mState >= GOT_HELLO)
        {
            AuthenticatedMessage am;
            xdr::xdr_from_msg(msg, am);
            recvMessage(am);
        }
        else
        {
            StellarMessage sm;
            xdr::xdr_from_msg(msg, sm);
            recvMessage(sm);
        }
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG(ERROR, "Overlay") << "received corrupt xdr::msg_ptr " << e.what();
        mDropInRecvMessageDecodeMeter.Mark();
        drop();
        return;
    }
}

bool
Peer::isConnected() const
{
    return mState != CONNECTING && mState != CLOSING;
}

bool
Peer::isAuthenticated() const
{
    return mState == GOT_AUTH;
}

bool
Peer::shouldAbort() const
{
    return (mState == CLOSING) || mApp.getOverlayManager().isShuttingDown();
}

void
Peer::recvMessage(AuthenticatedMessage const& msg)
{
    if (msg.sequence != mRecvMacSeq)
    {
        CLOG(ERROR, "Overlay") << "Unexpected message-auth sequence";
        mDropInRecvMessageSeqMeter.Mark();
        drop();
        return;
    }

    if (!hmacSha256Verify(msg.mac, mRecvMacKey,
                          xdr::xdr_to_opaque(msg.sequence,
                                             msg.message)))
    {
        CLOG(ERROR, "Overlay") << "Message-auth check failed";
        mDropInRecvMessageMacMeter.Mark();
        drop();
        return;
    }

    ++mRecvMacSeq;
    recvMessage(msg.message);
}

void
Peer::recvMessage(StellarMessage const& stellarMsg)
{
    CLOG(TRACE, "Overlay") << "("
                           << PubKeyUtils::toShortString(
                               mApp.getConfig().NODE_SEED.getPublicKey())
                           << ") recv: " << stellarMsg.type()
                           << " from:" << PubKeyUtils::toShortString(mPeerID);

    if (!isAuthenticated() &&
        (stellarMsg.type() != HELLO) &&
        (stellarMsg.type() != AUTH))
    {
        CLOG(WARNING, "Overlay") << "recv: " << stellarMsg.type()
                                 << " before completed handshake";
        mDropInRecvMessageUnauthMeter.Mark();
        drop();
        return;
    }

    assert(isAuthenticated() ||
           stellarMsg.type() == HELLO ||
           stellarMsg.type() == AUTH);

    switch (stellarMsg.type())
    {
    case ERROR_MSG:
    {
        auto t = mRecvErrorTimer.TimeScope();
        recvError(stellarMsg);
    }
    break;

    case HELLO:
    {
        auto t = mRecvHelloTimer.TimeScope();
        this->recvHello(stellarMsg);
    }
    break;

    case AUTH:
    {
        auto t = mRecvAuthTimer.TimeScope();
        this->recvAuth(stellarMsg);
    }
    break;

    case DONT_HAVE:
    {
        auto t = mRecvDontHaveTimer.TimeScope();
        recvDontHave(stellarMsg);
    }
    break;

    case GET_PEERS:
    {
        auto t = mRecvGetPeersTimer.TimeScope();
        recvGetPeers(stellarMsg);
    }
    break;

    case PEERS:
    {
        auto t = mRecvPeersTimer.TimeScope();
        recvPeers(stellarMsg);
    }
    break;

    case GET_TX_SET:
    {
        auto t = mRecvGetTxSetTimer.TimeScope();
        recvGetTxSet(stellarMsg);
    }
    break;

    case TX_SET:
    {
        auto t = mRecvTxSetTimer.TimeScope();
        recvTxSet(stellarMsg);
    }
    break;

    case TRANSACTION:
    {
        auto t = mRecvTransactionTimer.TimeScope();
        recvTransaction(stellarMsg);
    }
    break;

    case GET_SCP_QUORUMSET:
    {
        auto t = mRecvGetSCPQuorumSetTimer.TimeScope();
        recvGetSCPQuorumSet(stellarMsg);
    }
    break;

    case SCP_QUORUMSET:
    {
        auto t = mRecvSCPQuorumSetTimer.TimeScope();
        recvSCPQuorumSet(stellarMsg);
    }
    break;

    case SCP_MESSAGE:
    {
        auto t = mRecvSCPMessageTimer.TimeScope();
        recvSCPMessage(stellarMsg);
    }
    break;
    }
}

void
Peer::recvDontHave(StellarMessage const& msg)
{
    mApp.getHerder().peerDoesntHave(msg.dontHave().type, msg.dontHave().reqHash,
                                    shared_from_this());
}

void
Peer::recvGetTxSet(StellarMessage const& msg)
{
    auto self = shared_from_this();
    if (auto txSet = mApp.getHerder().getTxSet(msg.txSetHash()))
    {
        StellarMessage newMsg;
        newMsg.type(TX_SET);
        txSet->toXDR(newMsg.txSet());

        self->sendMessage(newMsg);
    }
    else
    {
        sendDontHave(TX_SET, msg.txSetHash());
    }
}

void
Peer::recvTxSet(StellarMessage const& msg)
{
    TxSetFrame frame(mApp.getNetworkID(), msg.txSet());
    mApp.getHerder().recvTxSet(frame.getContentsHash(), frame);
}

void
Peer::recvTransaction(StellarMessage const& msg)
{
    TransactionFramePtr transaction = TransactionFrame::makeTransactionFromWire(
        mApp.getNetworkID(), msg.transaction());
    if (transaction)
    {
        // add it to our current set
        // and make sure it is valid
        if (mApp.getHerder().recvTransaction(transaction) ==
            Herder::TX_STATUS_PENDING)
        {
            mApp.getOverlayManager().recvFloodedMsg(msg, shared_from_this());
            mApp.getOverlayManager().broadcastMessage(msg);
        }
    }
}

void
Peer::recvGetSCPQuorumSet(StellarMessage const& msg)
{
    SCPQuorumSetPtr qset = mApp.getHerder().getQSet(msg.qSetHash());

    if (qset)
    {
        sendSCPQuorumSet(qset);
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
    Hash hash = sha256(xdr::xdr_to_opaque(msg.qSet()));
    mApp.getHerder().recvSCPQuorumSet(hash, msg.qSet());
}

void
Peer::recvSCPMessage(StellarMessage const& msg)
{
    SCPEnvelope envelope = msg.envelope();
    CLOG(TRACE, "Overlay") << "recvSCPMessage node: "
                           << PubKeyUtils::toShortString(
                                  msg.envelope().statement.nodeID);

    mApp.getOverlayManager().recvFloodedMsg(msg, shared_from_this());

    mApp.getHerder().recvSCPEnvelope(envelope);
}

void
Peer::recvError(StellarMessage const& msg)
{
}

void
Peer::noteHandshakeSuccessInPeerRecord()
{
    auto pr = PeerRecord::loadPeerRecord(mApp.getDatabase(), getIP(),
                                         getRemoteListeningPort());
    if (pr)
    {
        pr->resetBackOff(mApp.getClock());
    }
    else
    {
        pr = make_optional<PeerRecord>(getIP(), mRemoteListeningPort,
                                       mApp.getClock().now());
    }
    CLOG(INFO, "Overlay") << "sucessful handshake with " << pr->toString();
    pr->storePeerRecord(mApp.getDatabase());
}

void
Peer::recvHello(StellarMessage const& msg)
{
    using xdr::operator==;

    if (msg.hello().overlayVersion != mApp.getConfig().OVERLAY_PROTOCOL_VERSION)
    {
        CLOG(ERROR, "Overlay")
            << "connection from peer with different overlay protocol version";
        CLOG(DEBUG, "Overlay")
            << "Protocol = " << msg.hello().overlayVersion
            << " expected: " << mApp.getConfig().OVERLAY_PROTOCOL_VERSION;
        mDropInRecvHelloVersionMeter.Mark();
        drop();
        return;
    }

    if (msg.hello().peerID == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        CLOG(WARNING, "Overlay") << "connecting to self";
        mDropInRecvHelloSelfMeter.Mark();
        drop();
        return;
    }

    if (msg.hello().networkID != mApp.getNetworkID())
    {
        CLOG(WARNING, "Overlay")
            << "connection from peer with different NetworkID";
        CLOG(DEBUG, "Overlay")
            << "NetworkID = " << hexAbbrev(msg.hello().networkID)
            << " expected: " << hexAbbrev(mApp.getNetworkID());
        mDropInRecvHelloNetMeter.Mark();
        drop();
        return;
    }

    if (msg.hello().listeningPort <= 0 ||
        msg.hello().listeningPort > UINT16_MAX)
    {
        CLOG(WARNING, "Overlay") << "bad port in recvHello";
        mDropInRecvHelloPortMeter.Mark();
        drop();
        return;
    }

    auto& peerAuth = mApp.getOverlayManager().getPeerAuth();
    if (!peerAuth.verifyRemoteAuthCert(msg.hello().peerID, msg.hello().cert))
    {
        CLOG(ERROR, "Overlay")
            << "failed to verify remote peer auth cert";
        mDropInRecvHelloCertMeter.Mark();
        drop();
        return;
    }

    mRemoteListeningPort =
        static_cast<unsigned short>(msg.hello().listeningPort);
    mRemoteOverlayVersion = msg.hello().overlayVersion;
    mRemoteVersion = msg.hello().versionStr;
    mPeerID = msg.hello().peerID;
    mRecvNonce = msg.hello().nonce;
    mSendMacSeq = 0;
    mRecvMacSeq = 0;
    mSendMacKey = peerAuth.getSendingMacKey(msg.hello().cert.pubkey,
                                            mSendNonce, mRecvNonce, mRole);
    mRecvMacKey = peerAuth.getReceivingMacKey(msg.hello().cert.pubkey,
                                              mSendNonce, mRecvNonce, mRole);

    mState = GOT_HELLO;
    CLOG(DEBUG, "Overlay") << "recvHello from " << toString();

    if (mRole == WE_CALLED_REMOTE)
    {
        sendAuth();
    }
    else
    {
        sendHello();
    }
}

void
Peer::recvAuth(StellarMessage const& msg)
{
    if (isAuthenticated())
    {
        CLOG(ERROR, "Overlay") << "Unexpected AUTH message";
        mDropInRecvAuthUnexpectedMeter.Mark();
        drop();
        return;
    }

    noteHandshakeSuccessInPeerRecord();
    mState = GOT_AUTH;

    if (mRole == REMOTE_CALLED_US)
    {
        sendAuth();
        sendPeers();
    }

    if (!mApp.getOverlayManager().isPeerAccepted(shared_from_this()))
    {
        CLOG(WARNING, "Overlay") << "New peer rejected, all slots taken";
        mDropInRecvAuthRejectMeter.Mark();
        drop();
        return;
    }
}

void
Peer::recvGetPeers(StellarMessage const& msg)
{
    sendPeers();
}

void
Peer::recvPeers(StellarMessage const& msg)
{
    for (auto const& peer : msg.peers())
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
                      mApp.getClock().now(), peer.numFailures};

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
