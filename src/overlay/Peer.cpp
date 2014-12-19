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
    stellarxdr::StellarMessage msg;
    msg.type(stellarxdr::HELLO);
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
Peer::sendDontHave(stellarxdr::MessageType type,
                   stellarxdr::uint256 const& itemID)
{
    stellarxdr::StellarMessage msg;
    msg.type(stellarxdr::DONT_HAVE);
    msg.dontHave().reqHash = itemID;
    msg.dontHave().type = type;

    sendMessage(msg);
}

void
Peer::sendQuorumSet(QuorumSet::pointer qSet)
{
    stellarxdr::StellarMessage msg;
    msg.type(stellarxdr::QUORUMSET);
    qSet->toXDR(msg.quorumSet());

    sendMessage(msg);
}
void
Peer::sendGetTxSet(stellarxdr::uint256 const& setID)
{
    stellarxdr::StellarMessage newMsg;
    newMsg.type(stellarxdr::GET_TX_SET);
    newMsg.txSetHash() = setID;

    sendMessage(newMsg);
}
void
Peer::sendGetQuorumSet(stellarxdr::uint256 const& setID)
{
    stellarxdr::StellarMessage newMsg;
    newMsg.type(stellarxdr::GET_QUORUMSET);
    newMsg.txSetHash() = setID;

    sendMessage(newMsg);
}

void
Peer::sendPeers()
{
    // LATER
}

void
Peer::sendMessage(stellarxdr::StellarMessage const& msg)
{
    CLOG(TRACE, "Overlay") << "sending stellarMessage";
    xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(msg));
    this->sendMessage(std::move(xdrBytes));
}

void
Peer::recvMessage(xdr::msg_ptr const& msg)
{
    CLOG(TRACE, "Overlay") << "received xdr::msg_ptr";
    stellarxdr::StellarMessage sm;
    xdr::xdr_from_msg(msg, sm);
    recvMessage(sm);
}

void
Peer::recvMessage(stellarxdr::StellarMessage const& stellarMsg)
{
    CLOG(TRACE, "Overlay") << "recv: " << stellarMsg.type();

    if (mState < GOT_HELLO && stellarMsg.type() != stellarxdr::HELLO)
    {
        CLOG(WARNING, "Overlay") << "recv: " << stellarMsg.type()
                                 << " before hello";
        drop();
        return;
    }

    switch (stellarMsg.type())
    {
    case stellarxdr::ERROR_MSG:
    {
        recvError(stellarMsg);
    }
    break;

    case stellarxdr::HELLO:
    {
        this->recvHello(stellarMsg);
    }
    break;

    case stellarxdr::DONT_HAVE:
    {
        recvDontHave(stellarMsg);
    }
    break;

    case stellarxdr::GET_PEERS:
    {
        recvGetPeers(stellarMsg);
    }
    break;

    case stellarxdr::PEERS:
    {
        recvPeers(stellarMsg);
    }
    break;

    case stellarxdr::GET_HISTORY:
    {
        recvGetHistory(stellarMsg);
    }
    break;

    case stellarxdr::HISTORY:
    {
        recvHistory(stellarMsg);
    }
    break;

    case stellarxdr::GET_DELTA:
    {
        recvGetDelta(stellarMsg);
    }
    break;

    case stellarxdr::DELTA:
    {
        recvDelta(stellarMsg);
    }
    break;

    case stellarxdr::GET_TX_SET:
    {
        recvGetTxSet(stellarMsg);
    }
    break;

    case stellarxdr::TX_SET:
    {
        recvTxSet(stellarMsg);
    }
    break;

    case stellarxdr::GET_VALIDATIONS:
    {
        recvGetValidations(stellarMsg);
    }
    break;

    case stellarxdr::VALIDATIONS:
    {
        recvValidations(stellarMsg);
    }
    break;

    case stellarxdr::TRANSACTION:
    {
        recvTransaction(stellarMsg);
    }
    break;

    case stellarxdr::GET_QUORUMSET:
    {
        recvGetQuorumSet(stellarMsg);
    }
    break;

    case stellarxdr::QUORUMSET:
    {
        recvQuorumSet(stellarMsg);
    }
    break;

    case stellarxdr::FBA_MESSAGE:
    {
        recvFBAMessage(stellarMsg);
    }
    break;
    case stellarxdr::JSON_TRANSACTION:
    {
        assert(false);
    }
    break;
    }
}

void
Peer::recvGetDelta(stellarxdr::StellarMessage const& msg)
{
    // LATER
}
void
Peer::recvDelta(stellarxdr::StellarMessage const& msg)
{
    // LATER
}

void
Peer::recvDontHave(stellarxdr::StellarMessage const& msg)
{
    switch (msg.dontHave().type)
    {
    case stellarxdr::HISTORY:
        // LATER
        break;
    case stellarxdr::DELTA:
        // LATER
        break;
    case stellarxdr::TX_SET:
        mApp.getTxHerderGateway().doesntHaveTxSet(msg.dontHave().reqHash,
                                                  shared_from_this());
        break;
    case stellarxdr::QUORUMSET:
        mApp.getOverlayGateway().doesntHaveQSet(msg.dontHave().reqHash,
                                                shared_from_this());
        break;
    case stellarxdr::VALIDATIONS:
    default:
        break;
    }
}

void
Peer::recvGetTxSet(stellarxdr::StellarMessage const& msg)
{
    TransactionSet::pointer txSet =
        mApp.getTxHerderGateway().fetchTxSet(msg.txSetHash(), false);
    if (txSet)
    {
        stellarxdr::StellarMessage newMsg;
        newMsg.type(stellarxdr::TX_SET);
        txSet->toXDR(newMsg.txSet());

        sendMessage(newMsg);
    }
    else
    {
        sendDontHave(stellarxdr::TX_SET, msg.txSetHash());
    }
}
void
Peer::recvTxSet(stellarxdr::StellarMessage const& msg)
{
    TransactionSet::pointer txSet =
        std::make_shared<TransactionSet>(msg.txSet());
    mApp.getTxHerderGateway().recvTransactionSet(txSet);
}

void
Peer::recvTransaction(stellarxdr::StellarMessage const& msg)
{
    Transaction::pointer transaction =
        Transaction::makeTransactionFromWire(msg.transaction());
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
Peer::recvGetQuorumSet(stellarxdr::StellarMessage const& msg)
{
    QuorumSet::pointer qset =
        mApp.getOverlayGateway().fetchQuorumSet(msg.qSetHash(), false);
    if (qset)
    {
        sendQuorumSet(qset);
    }
    else
    {
        sendDontHave(stellarxdr::QUORUMSET, msg.qSetHash());
        // do we want to ask other people for it?
    }
}
void
Peer::recvQuorumSet(stellarxdr::StellarMessage const& msg)
{
    QuorumSet::pointer qset =
        std::make_shared<QuorumSet>(msg.quorumSet(), mApp);
    mApp.getOverlayGateway().recvQuorumSet(qset);
}

void
Peer::recvFBAMessage(stellarxdr::StellarMessage const& msg)
{
    stellarxdr::FBAEnvelope envelope = msg.fbaMessage();
    Statement::pointer statement = std::make_shared<Statement>(envelope);

    mApp.getOverlayGateway().recvFloodedMsg(statement->mEnvelope.signature, msg,
                                            statement->getLedgerIndex(),
                                            shared_from_this());
    mApp.getFBAGateway().recvStatement(statement);
}

void
Peer::recvError(stellarxdr::StellarMessage const& msg)
{
    // LATER
}
void
Peer::recvHello(stellarxdr::StellarMessage const& msg)
{
    mRemoteProtocolVersion = msg.hello().protocolVersion;
    mRemoteVersion = msg.hello().versionStr;
    mRemoteListeningPort = msg.hello().port;
    CLOG(INFO, "Overlay") << "recvHello: " << mRemoteProtocolVersion << " "
                          << mRemoteVersion << " " << mRemoteListeningPort;
    mState = GOT_HELLO;
}
void
Peer::recvGetPeers(stellarxdr::StellarMessage const& msg)
{
    // LATER
}
void
Peer::recvPeers(stellarxdr::StellarMessage const& msg)
{
    // LATER
}
void
Peer::recvGetHistory(stellarxdr::StellarMessage const& msg)
{
    // LATER
}
void
Peer::recvHistory(stellarxdr::StellarMessage const& msg)
{
    // LATER
}

void
Peer::recvGetValidations(stellarxdr::StellarMessage const& msg)
{
    // LATER
}
void
Peer::recvValidations(stellarxdr::StellarMessage const& msg)
{
    // LATER
}
}
