#include "Peer.h"
#include "lib/util/Logging.h"
#include "main/Application.h"
#include "generated/stellar.hh"
#include "xdrpp/marshal.h"

#define MS_TO_WAIT_FOR_HELLO 2000

// LATER: need to add some way of docking peers that are misbehaving by sending you bad data

namespace stellar
{
	const char* Peer::kSQLCreateStatement = "CREATE TABLE IF NOT EXISTS Peers (						\
		peerID		INT PRIMARY KEY AUTO_INCREMENT,	\
		ip			varchar(16),			\
		port		INT,				\
		lastTry		timestamp,			\
		lastConnect	timestamp,		\
		rank	INT		\
	);";


	Peer::Peer(shared_ptr<asio::ip::tcp::socket> socket, Application::pointer app) :
        mHelloTimer(*(app->getPeerMaster().mIOservice))
	{
        mApp = app;
		mSocket = socket;
        mHelloTimer.expires_from_now(std::chrono::milliseconds(MS_TO_WAIT_FOR_HELLO));
        auto fun = std::bind(&Peer::neverSaidHello, this);
        mHelloTimer.async_wait(fun);
	}

	void Peer::createFromDoor()
	{
        sendHello();
	}


	void Peer::connect()
	{
		
        // GRAYDON mSocket->async_connect(server_endpoint, your_completion_handler);
		
	}

    // first of all, rude!
    void Peer::neverSaidHello()
    {
        drop();
    }

    

	void Peer::sendHello()
	{
		stellarxdr::StellarMessage msg;
        msg.type(stellarxdr::HELLO);
        msg.hello().protocolVersion = mApp->mConfig.PROTOCOL_VERSION;
        msg.hello().versionStr = mApp->mConfig.VERSION_STR;

		sendMessage(msg);
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


    void Peer::sendMessage(stellarxdr::StellarMessage& message)
    {
        StellarMessagePtr messagePtr = std::make_shared<stellarxdr::StellarMessage>(message);
        sendMessage(messagePtr);
	}

    void Peer::sendMessage(StellarMessagePtr message)
    {
        if(mState == CLOSING) {
            CLOG(TRACE, "Overlay") << "Peer::sendMessage while closing, dropping packet ";
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mOutputBufferMutex);
            mOutputBuffer.push_back(message);

            if(mOutputBuffer.size() == 1)
            {	// we aren't writing so write this guy
                reallySendMessage(message);
            }
        }
    }
		
    // GRAYDON
	void Peer::reallySendMessage(StellarMessagePtr message)
	{
        xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(*message.get()));

        mWriteBuffer.resize(xdrBytes->raw_size());
        memcpy(&(mWriteBuffer[0]),xdrBytes->raw_data(),mWriteBuffer.size());

		asio::async_write(*(mSocket.get()), asio::buffer(mWriteBuffer),
                          [this](std::error_code ec, std::size_t length) {
                              this->Peer::writeHandler(ec, length);
                          });
	}

	void Peer::connectHandler(const asio::error_code& error)
	{
		if(!error)
		{
			mState = CONNECTED;
			sendHello();
		} else
		{
            CLOG(WARNING, "Overlay") << "connectHandler error: " << error;
			// LATER drop Peer
		}
	}

	void Peer::writeHandler(const asio::error_code& error, std::size_t bytes_transferred)
	{
		if(!error)
		{
			std::lock_guard<std::mutex> lock(mOutputBufferMutex);
			mOutputBuffer.pop_front();

			if(mOutputBuffer.size()) reallySendMessage(mOutputBuffer[0]);
		} else
		{
            CLOG(WARNING, "Overlay") << "writeHandler error: " << error;
			// LATER drop Peer
		}
		
	}

	void Peer::startRead()
	{
        //boost::shared_ptr<asio::ip::tcp::socket> test;

		asio::async_read(*(mSocket.get()), asio::buffer(mIncomingHeader),
                         [this](std::error_code ec, std::size_t length) {
                             this->Peer::readHeaderHandler(ec, length);
                         });
	}

    // GRAYDON
    int Peer::getIncomingMsgLength()
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
    // GRAYDON
	void Peer::readHeaderHandler(const asio::error_code& error, std::size_t bytes_transferred)
	{
		if(!error)
		{
            mIncomingBody.resize(getIncomingMsgLength());
            
			asio::async_read(*mSocket.get(), asio::buffer(mIncomingBody),
                             [this](std::error_code ec, std::size_t length) {
                                 this->Peer::readBodyHandler(ec, length);
                             });
		} else
		{
            CLOG(WARNING, "Overlay") << "readHeaderHandler error: " << error;
			// LATER drop Peer
		}
		
	}

	void Peer::readBodyHandler(const asio::error_code& error, std::size_t bytes_transferred)
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

    // GRAYDON
    // disconnect from peer
    void Peer::drop()
    {

    }

    // GRAYDON
	void Peer::recvMessage()
	{
        StellarMessagePtr stellarMsg = std::make_shared<stellarxdr::StellarMessage>();

        xdr::msg_ptr incoming=xdr::message_t::alloc(mIncomingBody.size());
        memcpy(incoming->raw_data(),&(mIncomingBody[0]), mIncomingBody.size());
        xdr::xdr_from_msg(incoming, *stellarMsg.get());

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
                recvHello(stellarMsg);
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
            mApp->getTxHerderGateway().doesntHaveTxSet(msg->dontHave().reqHash, shared_from_this());
            break;
        case stellarxdr::QUORUMSET:
            mApp->getOverlayGateway().doesntHaveQSet(msg->dontHave().reqHash, shared_from_this());
            break;
        case stellarxdr::VALIDATIONS:
        default:
            break;
        }
    }

	void Peer::recvGetTxSet(StellarMessagePtr msg)
	{
        TransactionSet::pointer txSet = mApp->getTxHerderGateway().fetchTxSet(msg->txSetHash(),false);
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
        mApp->getTxHerderGateway().recvTransactionSet(txSet);
	}


	void Peer::recvTransaction(StellarMessagePtr msg)
	{
		Transaction::pointer transaction = Transaction::makeTransactionFromWire(msg->transaction());
		if(transaction)
		{
			if(mApp->getTxHerderGateway().recvTransaction(transaction))   // add it to our current set
			{
                mApp->getOverlayGateway().broadcastMessage(msg, shared_from_this());
			}
		}
	}

	void Peer::recvGetQuorumSet(StellarMessagePtr msg)
	{
		QuorumSet::pointer qset= mApp->getOverlayGateway().fetchQuorumSet(msg->qSetHash(),false);
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
        mApp->getOverlayGateway().recvQuorumSet(qset);

	}

	void Peer::recvFBAMessage(StellarMessagePtr msg)
	{
        stellarxdr::FBAEnvelope envelope=msg->fbaMessage();
        Statement::pointer statement = Statement::makeStatement(envelope);

        mApp->getOverlayGateway().recvFloodedMsg(statement->mSignature, msg, statement->getLedgerIndex(), shared_from_this());
        mApp->getFBAGateway().recvStatement(statement);
	}

    void Peer::recvError(StellarMessagePtr msg)
    {
        // LATER
    }
    void Peer::recvHello(StellarMessagePtr msg)
    {
        mHelloTimer.cancel();
        mProtocolVersion=msg->hello().protocolVersion;
        mVersion = msg->hello().versionStr;
        mPort = msg->hello().port;

        mState = GOT_HELLO;

        if(! mApp->getPeerMaster().isPeerAccepted(shared_from_this()))
        {  // we can't accept anymore peer connections
            sendPeers();
            drop();
        }
        
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

}
