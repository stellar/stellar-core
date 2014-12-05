#include "Peer.h"
#include "lib/util/Logging.h"
#include "main/Application.h"
#include "generated/stellar.hh"
#include "xdrpp/marshal.h"
#include "overlay/PeerMaster.h"

#define MS_TO_WAIT_FOR_HELLO 2000

// LATER: need to add some way of docking peers that are misbehaving by sending you bad data

namespace stellar
{
    Peer::Peer(Application &app, PeerRole role)
        : mApp(app)
        , mRole(role)
        , mState(role == ACCEPTOR ? CONNECTED : CONNECTING)
        , mRemoteListeningPort(-1)
    {
        if (mRole == ACCEPTOR)
        {
            // Schedule a 'say hello' event at the next opportunity,
            // if we're the acceptor-role.
            mApp.getMainIOService().post([this](){
                    this->sendHello();
                });
        }
    }

    void Peer::sendHello()
    {
        stellarxdr::StellarMessage msg;
        msg.type(stellarxdr::HELLO);
        msg.hello().protocolVersion = mApp.mConfig.PROTOCOL_VERSION;
        msg.hello().versionStr = mApp.mConfig.VERSION_STR;

        sendMessage(msg);
    }

    void Peer::connectHandler(const asio::error_code& error)
    {
        if(error)
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

    void Peer::sendDontHave(stellarxdr::MessageType type, stellarxdr::uint256& itemID)
    {
        stellarxdr::StellarMessage msg;
        msg.type(stellarxdr::DONT_HAVE);
        msg.dontHave().reqHash = itemID;
        msg.dontHave().type = type;

        sendMessage(msg);
    }

    void Peer::sendQuorumSet(QuorumSet::pointer qSet)
    {
        stellarxdr::StellarMessage msg;
        msg.type(stellarxdr::QUORUMSET);
        qSet->toXDR(msg.quorumSet());

        sendMessage(msg);
    }
    void Peer::sendGetTxSet(stellarxdr::uint256& setID)
    {
        stellarxdr::StellarMessage newMsg;
        newMsg.type(stellarxdr::GET_TX_SET);
        newMsg.txSetHash()=setID;

        sendMessage(newMsg);
    }
    void Peer::sendGetQuorumSet(stellarxdr::uint256& setID)
    {
        stellarxdr::StellarMessage newMsg;
        newMsg.type(stellarxdr::GET_QUORUMSET);
        newMsg.txSetHash()=setID;

        sendMessage(newMsg);
    }

    void Peer::sendPeers()
    {
        // LATER
    }

    void Peer::sendMessage(stellarxdr::StellarMessage msg)
    {
        CLOG(TRACE, "Overlay") << "sending stellarMessage";
        xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(msg));
        this->sendMessage(std::move(xdrBytes));
    }

    void Peer::recvMessage(xdr::msg_ptr const& msg)
    {
        CLOG(TRACE, "Overlay") << "received xdr::msg_ptr";
        StellarMessagePtr stellarMsg = std::make_shared<stellarxdr::StellarMessage>();
        xdr::xdr_from_msg(msg, *stellarMsg.get());
        recvMessage(stellarMsg);
    }

    void Peer::recvMessage(StellarMessagePtr stellarMsg)
    {
        CLOG(TRACE, "Overlay") << "recv: " << stellarMsg->type();

        if(mState < GOT_HELLO && stellarMsg->type() != stellarxdr::HELLO)
        {
            CLOG(WARNING, "Overlay") << "recv: " << stellarMsg->type() << " before hello";
            drop();
            return;
        }

        switch(stellarMsg->type())
        {
            case stellarxdr::ERROR_MSG:
            {
                recvError(stellarMsg);
            }break;

            case stellarxdr::HELLO:
            {
                this->recvHello(stellarMsg);
            }break;

            case stellarxdr::DONT_HAVE:
            {
                recvDontHave(stellarMsg);
            }break;

            case stellarxdr::GET_PEERS:
            {
                recvGetPeers(stellarMsg);
            }break;

            case stellarxdr::PEERS:
            {
                recvPeers(stellarMsg);
            }break;

            case stellarxdr::GET_HISTORY:
            {
                recvGetHistory(stellarMsg);
            }break;

            case stellarxdr::HISTORY:
            {
                recvHistory(stellarMsg);
            }break;

            case stellarxdr::GET_DELTA:
            {
                recvGetDelta(stellarMsg);
            }break;

            case stellarxdr::DELTA:
            {
                recvDelta(stellarMsg);
            }break;

            case stellarxdr::GET_TX_SET:
            {
                recvGetTxSet(stellarMsg);
            }break;

            case stellarxdr::TX_SET:
            {
                recvTxSet(stellarMsg);
            }break;

            case stellarxdr::GET_VALIDATIONS:
            {
                recvGetValidations(stellarMsg);
            }break;

            case stellarxdr::VALIDATIONS:
            {
                recvValidations(stellarMsg);
            }break;

            case stellarxdr::TRANSACTION:
            {
                recvTransaction(stellarMsg);
            }break;

            case stellarxdr::GET_QUORUMSET:
            {
                recvGetQuorumSet(stellarMsg);
            }break;

            case stellarxdr::QUORUMSET:
            {
                recvQuorumSet(stellarMsg);
            }break;

            case stellarxdr::FBA_MESSAGE:
            {
                recvFBAMessage(stellarMsg);
            }break;
            case stellarxdr::JSON_TRANSACTION:
            {
                assert(false);
            }break;
        }
    }

    void Peer::recvGetDelta(StellarMessagePtr msg)
    {
        // LATER
    }
    void Peer::recvDelta(StellarMessagePtr msg)
    {
        // LATER
    }

    void Peer::recvDontHave(StellarMessagePtr msg)
    {
        switch(msg->dontHave().type)
        {
        case stellarxdr::HISTORY:
            // LATER
            break;
        case stellarxdr::DELTA:
            // LATER
            break;
        case stellarxdr::TX_SET:
            mApp.getTxHerderGateway().doesntHaveTxSet(msg->dontHave().reqHash, shared_from_this());
            break;
        case stellarxdr::QUORUMSET:
            mApp.getOverlayGateway().doesntHaveQSet(msg->dontHave().reqHash, shared_from_this());
            break;
        case stellarxdr::VALIDATIONS:
        default:
            break;
        }
    }

    void Peer::recvGetTxSet(StellarMessagePtr msg)
    {
        TransactionSet::pointer txSet = mApp.getTxHerderGateway().fetchTxSet(msg->txSetHash(),false);
        if(txSet)
        {
            stellarxdr::StellarMessage newMsg;
            newMsg.type(stellarxdr::TX_SET);
            txSet->toXDR(newMsg.txSet());

            sendMessage(newMsg);
        } else
        {
            sendDontHave(stellarxdr::TX_SET, msg->txSetHash());
        }
    }
    void Peer::recvTxSet(StellarMessagePtr msg)
    {
        TransactionSet::pointer txSet = std::make_shared<TransactionSet>(msg->txSet());
        mApp.getTxHerderGateway().recvTransactionSet(txSet);
    }


    void Peer::recvTransaction(StellarMessagePtr msg)
    {
        Transaction::pointer transaction = Transaction::makeTransactionFromWire(msg->transaction());
        if(transaction)
        {
            if(mApp.getTxHerderGateway().recvTransaction(transaction))   // add it to our current set
            {
                mApp.getOverlayGateway().broadcastMessage(msg, shared_from_this());
            }
        }
    }

    void Peer::recvGetQuorumSet(StellarMessagePtr msg)
    {
        QuorumSet::pointer qset= mApp.getOverlayGateway().fetchQuorumSet(msg->qSetHash(),false);
        if(qset)
        {
            sendQuorumSet(qset);
        } else
        {
            sendDontHave(stellarxdr::QUORUMSET, msg->qSetHash());
            // do we want to ask other people for it?
        }

    }
    void Peer::recvQuorumSet(StellarMessagePtr msg)
    {
        QuorumSet::pointer qset = std::make_shared<QuorumSet>(msg->quorumSet(),mApp);
        mApp.getOverlayGateway().recvQuorumSet(qset);

    }

    void Peer::recvFBAMessage(StellarMessagePtr msg)
    {
        stellarxdr::FBAEnvelope envelope=msg->fbaMessage();
        Statement::pointer statement = Statement::makeStatement(envelope);

        mApp.getOverlayGateway().recvFloodedMsg(statement->mSignature, msg, statement->getLedgerIndex(), shared_from_this());
        mApp.getFBAGateway().recvStatement(statement);
    }

    void Peer::recvError(StellarMessagePtr msg)
    {
        // LATER
    }
    void Peer::recvHello(StellarMessagePtr msg)
    {
        mRemoteProtocolVersion = msg->hello().protocolVersion;
        mRemoteVersion = msg->hello().versionStr;
        mRemoteListeningPort = msg->hello().port;
        CLOG(INFO, "Overlay") << "recvHello: "
                              << mRemoteProtocolVersion << " "
                              << mRemoteVersion << " "
                              << mRemoteListeningPort;
        mState = GOT_HELLO;
    }
    void Peer::recvGetPeers(StellarMessagePtr msg)
    {
        // LATER
    }
    void Peer::recvPeers(StellarMessagePtr msg)
    {
        // LATER
    }
    void Peer::recvGetHistory(StellarMessagePtr msg)
    {
        // LATER
    }
    void Peer::recvHistory(StellarMessagePtr msg)
    {
        // LATER
    }

    void Peer::recvGetValidations(StellarMessagePtr msg)
    {
        // LATER
    }
    void Peer::recvValidations(StellarMessagePtr msg)
    {
        // LATER
    }


    ///////////////////////////////////////////////////////////////////////
    // TCPPeer
    ///////////////////////////////////////////////////////////////////////


    const char* TCPPeer::kSQLCreateStatement = "CREATE TABLE IF NOT EXISTS Peers (                      \
        peerID      INT PRIMARY KEY AUTO_INCREMENT, \
        ip          varchar(16),            \
        port        INT,                \
        lastTry     timestamp,          \
        lastConnect timestamp,      \
        rank    INT     \
    );";

    TCPPeer::TCPPeer(Application &app,
                     shared_ptr<asio::ip::tcp::socket> socket,
                     PeerRole role)
        : Peer(app, role)
        , mSocket(socket)
        , mHelloTimer(app.getMainIOService())
    {
        mHelloTimer.expires_from_now(std::chrono::milliseconds(MS_TO_WAIT_FOR_HELLO));
        mHelloTimer.async_wait([socket](asio::error_code const& ec) {
                socket->shutdown(asio::socket_base::shutdown_both);
                socket->close();
            });
    }

    std::string TCPPeer::getIP() {
        return mSocket->remote_endpoint().address().to_string();
    }

    void TCPPeer::connect()
    {
        // GRAYDON mSocket->async_connect(server_endpoint, your_completion_handler);
    }

    void TCPPeer::sendMessage(xdr::msg_ptr &&xdrBytes)
    {
        using std::placeholders::_1;
        using std::placeholders::_2;

        // Pass ownership of a serizlied XDR message buffer, along with an
        // asio::buffer pointing into it, to the callback for async_write, so it
        // survives as long as the request is in flight in the io_service, and
        // is deallocated when the write completes.
        //
        // The messy bind-of-lambda expression is required due to C++11 not
        // supporting moving (passing ownership) into a lambda capture. This is
        // fixed in C++14 but we're not there yet.

        auto self = shared_from_this();
        asio::async_write(*(mSocket.get()),
                          asio::buffer(xdrBytes->raw_data(),
                                       xdrBytes->raw_size()),
                          std::bind([self](asio::error_code const &ec,
                                           std::size_t length,
                                           xdr::msg_ptr const&) {
                                        self->writeHandler(ec, length);
                                    }, _1, _2, std::move(xdrBytes)));
    }


    void TCPPeer::writeHandler(const asio::error_code& error, std::size_t bytes_transferred)
    {
        if(error)
        {
            CLOG(WARNING, "Overlay") << "writeHandler error: " << error;
            // LATER drop Peer
        }

    }

    void TCPPeer::startRead()
    {
        auto self = shared_from_this();
        asio::async_read(*(mSocket.get()), asio::buffer(mIncomingHeader),
                         [self](std::error_code ec, std::size_t length) {
                             self->Peer::readHeaderHandler(ec, length);
                         });
    }

    int TCPPeer::getIncomingMsgLength()
    {
        int length = mIncomingHeader[0];
        length <<= 8;
        length |= mIncomingHeader[1];
        length <<= 8;
        length |= mIncomingHeader[2];
        length <<= 8;
        length |= mIncomingHeader[3];
        return(length);
    }

    void TCPPeer::readHeaderHandler(const asio::error_code& error, std::size_t bytes_transferred)
    {
        if(!error)
        {
            mIncomingBody.resize(getIncomingMsgLength());
            auto self = shared_from_this();
            asio::async_read(*mSocket.get(), asio::buffer(mIncomingBody),
                             [self](std::error_code ec, std::size_t length) {
                                 self->Peer::readBodyHandler(ec, length);
                             });
        } else
        {
            CLOG(WARNING, "Overlay") << "readHeaderHandler error: " << error;
            // LATER drop Peer
        }
    }

    void TCPPeer::readBodyHandler(const asio::error_code& error, std::size_t bytes_transferred)
    {
        if(!error)
        {
            recvMessage();
            startRead();
        }else
        {
            CLOG(WARNING, "Overlay") << "readBodyHandler error: " << error;
            // LATER drop Peer
        }

    }

    void TCPPeer::recvMessage()
    {
        // FIXME: This can do one-less-copy, given a new unmarshal-from-raw-pointers
        // helper in xdrpp.
        xdr::msg_ptr incoming = xdr::message_t::alloc(mIncomingBody.size());
        memcpy(incoming->raw_data(), mIncomingBody.data(), mIncomingBody.size());
        Peer::recvMessage(std::move(incoming));
    }

    void TCPPeer::recvHello(StellarMessagePtr msg)
    {
        mHelloTimer.cancel();
        Peer::recvHello(msg);
        if(!mApp.getPeerMaster().isPeerAccepted(shared_from_this()))
        {  // we can't accept anymore peer connections
            sendPeers();
            drop();
        }
    }

    void TCPPeer::drop()
    {
        auto sock = mSocket;
        mApp.getMainIOService().post([sock](){
                sock->shutdown(asio::socket_base::shutdown_both);
                sock->close();
            });
    }



    ///////////////////////////////////////////////////////////////////////
    // LoopbackPeer
    ///////////////////////////////////////////////////////////////////////


    LoopbackPeer::LoopbackPeer(Application& app, PeerRole role)
        : Peer(app, role)
        , mRemote(nullptr)
    {}

    void LoopbackPeer::sendMessage(xdr::msg_ptr&& xdrBytes)
    {
        if (mRemote) {
            // Pass ownership of a serizlied XDR message buffer to a recvMesage
            // callback event against the remote Peer, posted on the remote
            // Peer's io_service.
            auto remote = mRemote;
            remote->getApp().getMainIOService().post(
                std::bind([remote](xdr::msg_ptr const& msg){
                        remote->recvMessage(msg);
                    }, std::move(xdrBytes)));
        }
    }

    std::string LoopbackPeer::getIP() {
        return "<loopback>";
    }

    void LoopbackPeer::drop()
    {
        if (mRemote) {
            auto remote = mRemote;
            mRemote->getApp().getMainIOService().post([remote]() {
                    remote->mRemote = nullptr;
                });
            mRemote = nullptr;
        }
    }

    void LoopbackPeer::connectApplications(Application &initiator,
                                           Application &acceptor)
    {

        auto iniPeer = make_shared<LoopbackPeer>(initiator, Peer::INITIATOR);
        auto accPeer = make_shared<LoopbackPeer>(acceptor, Peer::ACCEPTOR);

        iniPeer->mRemote = accPeer;
        iniPeer->mState = Peer::CONNECTED;

        accPeer->mRemote = iniPeer;
        accPeer->mState = Peer::CONNECTED;

        initiator.getPeerMaster().addPeer(accPeer);
        acceptor.getPeerMaster().addPeer(iniPeer);
    }


}
