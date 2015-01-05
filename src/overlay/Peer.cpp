// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Peer.h"

#include "util/Logging.h"
#include "main/Application.h"
#include "generated/StellarXDR.h"
#include "xdrpp/marshal.h"
#include "overlay/PeerMaster.h"

// LATER: need to add some way of docking peers that are misbehaving by sending
// you bad data

namespace stellar
{
Peer::Peer(Application& app, PeerRole role)
    : mApp(app)
    , mRole(role)
    , mState(role == ACCEPTOR ? CONNECTED : CONNECTING)
    , mRemoteListeningPort(-1)
{
    if (mRole == ACCEPTOR)
    {
        // Schedule a 'say hello' event at the next opportunity,
        // if we're the acceptor-role.
        mApp.getMainIOService().post([this]()
                                     {
                                         this->sendHello();
                                     });
    }
}

void
Peer::sendHello()
{
    StellarMessage msg;
    msg.type(HELLO);
    msg.hello().protocolVersion = mApp.getConfig().PROTOCOL_VERSION;
    msg.hello().versionStr = mApp.getConfig().VERSION_STR;

    sendMessage(msg);
}

void
Peer::connectHandler(const asio::error_code& error)
{
    if (error)
    {
        CLOG(WARNING, "Overlay") << "connectHandler error: " << error;
        drop();
    }
    else
    {
        mState = CONNECTED;
        sendHello();
    }
}

void
Peer::sendDontHave(MessageType type,
                   uint256 const& itemID)
{
    StellarMessage msg;
    msg.type(DONT_HAVE);
    msg.dontHave().reqHash = itemID;
    msg.dontHave().type = type;

    sendMessage(msg);
}

void
Peer::sendFBAQuorumSet(FBAQuorumSetPtr qSet)
{
    StellarMessage msg;
    msg.type(FBA_QUORUMSET);
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
    StellarMessage newMsg;
    newMsg.type(GET_FBA_QUORUMSET);
    newMsg.txSetHash() = setID;

    sendMessage(newMsg);
}

void
Peer::sendPeers()
{
    // TODO.3
}

void
Peer::sendMessage(StellarMessage const& msg)
{
    CLOG(TRACE, "Overlay") << "sending stellarMessage";
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
    CLOG(TRACE, "Overlay") << "recv: " << stellarMsg.type();

    if (mState < GOT_HELLO && stellarMsg.type() != HELLO)
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

    case GET_HISTORY:
    {
        recvGetHistory(stellarMsg);
    }
    break;

    case HISTORY:
    {
        recvHistory(stellarMsg);
    }
    break;

    case GET_DELTA:
    {
        recvGetDelta(stellarMsg);
    }
    break;

    case DELTA:
    {
        recvDelta(stellarMsg);
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

    case GET_VALIDATIONS:
    {
        recvGetValidations(stellarMsg);
    }
    break;

    case VALIDATIONS:
    {
        recvValidations(stellarMsg);
    }
    break;

    case TRANSACTION:
    {
        recvTransaction(stellarMsg);
    }
    break;

    case GET_FBA_QUORUMSET:
    {
        recvGetFBAQuorumSet(stellarMsg);
    }
    break;

    case FBA_QUORUMSET:
    {
        recvFBAQuorumSet(stellarMsg);
    }
    break;

    case FBA_MESSAGE:
    {
        recvFBAMessage(stellarMsg);
    }
    break;
    case JSON_TRANSACTION:
    {
        assert(false);
    }
    break;
    }
}

void
Peer::recvGetDelta(StellarMessage const& msg)
{
    // LATER
}
void
Peer::recvDelta(StellarMessage const& msg)
{
    // LATER
}

void
Peer::recvDontHave(StellarMessage const& msg)
{
    switch (msg.dontHave().type)
    {
    case HISTORY:
        // LATER
        break;
    case DELTA:
        // LATER
        break;
    case TX_SET:
        mApp.getTxHerderGateway().doesntHaveTxSet(msg.dontHave().reqHash,
                                                  shared_from_this());
        break;
    case FBA_QUORUMSET:
        mApp.getOverlayGateway().doesntHaveQSet(msg.dontHave().reqHash,
                                                shared_from_this());
        break;
    case VALIDATIONS:
    default:
        break;
    }
}

void
Peer::recvGetTxSet(StellarMessage const& msg)
{
    TxSetFramePtr txSet =
        mApp.getTxHerderGateway().fetchTxSet(msg.txSetHash(), false);
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
    TxSetFramePtr txSet =
        std::make_shared<TxSetFrame>(msg.txSet());
    mApp.getTxHerderGateway().recvTransactionSet(txSet);
}

void
Peer::recvTransaction(StellarMessage const& msg)
{
    TransactionFramePtr transaction =
        TransactionFrame::makeTransactionFromWire(msg.transaction());
    if (transaction)
    {
        if (mApp.getTxHerderGateway().recvTransaction(
                transaction)) // add it to our current set
        {
            mApp.getOverlayGateway().broadcastMessage(msg, shared_from_this());
        }
    }
}

void
Peer::recvGetFBAQuorumSet(StellarMessage const& msg)
{
    FBAQuorumSetPtr qset = 
        mApp.getOverlayGateway().fetchFBAQuorumSet(msg.qSetHash(), false);
    if (qset->content.type() != FBAQuorumSetType::UNKNOWN)
    {
        sendFBAQuorumSet(qset);
    }
    else
    {
        sendDontHave(FBA_QUORUMSET, msg.qSetHash());
        // do we want to ask other people for it?
    }
}
void
Peer::recvFBAQuorumSet(StellarMessage const& msg)
{
  FBAQuorumSetPtr qSet =
    std::make_shared<FBAQuorumSet>(msg.qSet());
    mApp.getOverlayGateway().recvFBAQuorumSet(qSet);
}

void
Peer::recvFBAMessage(StellarMessage const& msg)
{
    /* TODO(spolu) Update to new FBA */
    FBAEnvelope envelope = msg.fbaMessage();
    /*
    Statement::pointer statement = std::make_shared<Statement>(envelope);

    mApp.getOverlayGateway().recvFloodedMsg(statement->mContentsHash, msg,
                                            statement->getLedgerIndex(),
                                            shared_from_this());
    mApp.getFBAGateway().recvStatement(statement);
    */
}

void
Peer::recvError(StellarMessage const& msg)
{
    // TODO.3
}
void
Peer::recvHello(StellarMessage const& msg)
{
    mRemoteProtocolVersion = msg.hello().protocolVersion;
    mRemoteVersion = msg.hello().versionStr;
    mRemoteListeningPort = msg.hello().port;
    CLOG(INFO, "Overlay") << "recvHello: " << mRemoteProtocolVersion << " "
                          << mRemoteVersion << " " << mRemoteListeningPort;
    mState = GOT_HELLO;
}
void
Peer::recvGetPeers(StellarMessage const& msg)
{
    // TODO.3
}
void
Peer::recvPeers(StellarMessage const& msg)
{
    // TODO.3
}
void
Peer::recvGetHistory(StellarMessage const& msg)
{
    // LATER
}
void
Peer::recvHistory(StellarMessage const& msg)
{
    // LATER
}

void
Peer::recvGetValidations(StellarMessage const& msg)
{
    // TODO.3
}
void
Peer::recvValidations(StellarMessage const& msg)
{
    // TODO.3
}
}
