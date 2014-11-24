#include <boost/bind.hpp>
#include "Peer.h"
#include "lib/util/Logging.h"
#include "main/Application.h"
#include "generated/stellar.hh"
#include "xdrpp/marshal.h"

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


	Peer::Peer(shared_ptr<boost::asio::ip::tcp::socket> socket)
	{
		mSocket = socket;
	}

	void Peer::createFromDoor()
	{
		sendHello();
	}


	void Peer::connect()
	{
		
        // SANITY mSocket->async_connect(server_endpoint, your_completion_handler);
		
	}

	void Peer::sendHello()
	{
		stellarxdr::StellarMessage msg;
        msg.type(stellarxdr::HELLO);
        msg.hello().protocolVersion = gApp.mConfig.PROTOCOL_VERSION;
        msg.hello().versionStr = gApp.mConfig.VERSION_STR;

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

    void Peer::sendMessage(stellarxdr::StellarMessage& message)
    {
        StellarMessagePtr messagePtr(new stellarxdr::StellarMessage(message));
        sendMessage(messagePtr);
	}

    void Peer::sendMessage(StellarMessagePtr message)
    {
        if(mState == CLOSING) {
            // SANITY if(ShouldLog(lsTRACE, stellar::PeerMaster))
            // SANITY WriteLog(lsTRACE, stellar::PeerMaster) << "Peer::sendMessage while closing, dropping packet "
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

		boost::asio::async_write(
            *(mSocket.get()), boost::asio::buffer(mWriteBuffer), bind(&Peer::writeHandler, this,
			boost::asio::placeholders::error,
			boost::asio::placeholders::bytes_transferred
			));
	}

	void Peer::connectHandler(const boost::system::error_code& error)
	{
		if(!error)
		{
			mState = CONNECTED;
			sendHello();
		} else
		{
            // SANITY WriteLog(lsWARNING, stellar::PeerMaster) << "connectHandler error: " << error;
			// SANITY drop Peer
		}
	}

	void Peer::writeHandler(const boost::system::error_code& error, std::size_t bytes_transferred)
	{
		if(!error)
		{
			std::lock_guard<std::mutex> lock(mOutputBufferMutex);
			mOutputBuffer.pop_front();

			if(mOutputBuffer.size()) reallySendMessage(mOutputBuffer[0]);
		} else
		{
            // SANITY WriteLog(lsWARNING, stellar::PeerMaster) << "writeHandler error: " << error;
			// SANITY drop Peer
		}
		
	}

	void Peer::startRead()
	{
        //boost::shared_ptr<boost::asio::ip::tcp::socket> test;

		boost::asio::async_read(*(mSocket.get()), boost::asio::buffer(mIncomingHeader), bind(&Peer::readHeaderHandler, this,
			boost::asio::placeholders::error,
			boost::asio::placeholders::bytes_transferred
			));
            
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
	void Peer::readHeaderHandler(const boost::system::error_code& error, std::size_t bytes_transferred)
	{
		if(!error)
		{
            mIncomingBody.resize(getIncomingMsgLength());
            
			boost::asio::async_read(*mSocket.get(), boost::asio::buffer(mIncomingBody), bind(&Peer::readBodyHandler, this,
				boost::asio::placeholders::error,
				boost::asio::placeholders::bytes_transferred
				)); 
		} else
		{
            // SANITY WriteLog(lsWARNING, stellar::PeerMaster) << "readHeaderHandler error: " << error;
			// SANITY drop Peer
		}
		
	}

	void Peer::readBodyHandler(const boost::system::error_code& error, std::size_t bytes_transferred)
	{
		if(!error)
		{
			recvMessage();
			startRead();
		}else
		{
            // SANITY WriteLog(lsWARNING, stellar::PeerMaster) << "readBodyHandler error: " << error;
			// SANITY drop Peer
		}
		
	}

    // GRAYDON
	void Peer::recvMessage()
	{
        StellarMessagePtr stellarMsg(new stellarxdr::StellarMessage());

        xdr::msg_ptr incoming=xdr::message_t::alloc(mIncomingBody.size());
        memcpy(incoming->raw_data(),&(mIncomingBody[0]), mIncomingBody.size());
        xdr::xdr_from_msg(incoming, *stellarMsg.get());

        // SANITY WriteLog(lsTRACE, stellar::PeerMaster) << "recv: " << stellarMsg.type();

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
		}	
	}

	void Peer::recvGetDelta(StellarMessagePtr msg)
	{
        // SANITY
	}
	void Peer::recvDelta(StellarMessagePtr msg)
	{
        // SANITY
	}

    void Peer::recvDontHave(StellarMessagePtr msg)
    {
        switch(msg->dontHave().type)
        {
        case stellarxdr::HISTORY:
            // SANITY
            break;
        case stellarxdr::DELTA:
            // SANITY
            break;
        case stellarxdr::TX_SET:
            gApp.getTxHerderGateway().doesntHaveTxSet(msg->dontHave().reqHash, shared_from_this());
            break;
        case stellarxdr::QUORUMSET:
            gApp.getOverlayGateway().doesntHaveQSet(msg->dontHave().reqHash, shared_from_this());
            break;
        case stellarxdr::VALIDATIONS:
            break;
        }
    }

	void Peer::recvGetTxSet(StellarMessagePtr msg)
	{
        TransactionSet::pointer txSet = gApp.getTxHerderGateway().fetchTxSet(msg->txSetHash());
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
		TransactionSet::pointer txSet(new TransactionSet(msg->txSet()));
		gApp.getTxHerderGateway().recvTransactionSet(txSet);
	}


	void Peer::recvTransaction(StellarMessagePtr msg)
	{
		Transaction::pointer transaction = Transaction::makeTransactionFromWire(msg->transaction());
		if(transaction)
		{
			if(gApp.getTxHerderGateway().recvTransaction(transaction))   // add it to our current set
			{
				gApp.getOverlayGateway().broadcastMessage(msg, shared_from_this());
			}
		}
	}

	void Peer::recvGetQuorumSet(StellarMessagePtr msg)
	{
		QuorumSet::pointer qset=gApp.getOverlayGateway().fetchQuorumSet(msg->qSetHash());
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
		QuorumSet::pointer qset(new QuorumSet(msg->quorumSet()));
		gApp.getOverlayGateway().recvQuorumSet(qset);

	}

	void Peer::recvFBAMessage(StellarMessagePtr msg)
	{
        stellarxdr::FBAEnvelope envelope=msg->fbaMessage();
        Statement::pointer statement = Statement::makeStatement(envelope);

		gApp.getOverlayGateway().recvFloodedMsg(statement->mSignature, msg, statement->getLedgerIndex(), shared_from_this());
		gApp.getFBAGateway().recvStatement(statement);
	}

    void Peer::recvError(StellarMessagePtr msg)
    {
        // LATER
    }
    void Peer::recvHello(StellarMessagePtr msg)
    {
        // LATER
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