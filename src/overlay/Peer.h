#ifndef __PEER__
#define __PEER__

#include <boost/asio.hpp>
#include <deque>
#include <mutex>
#include "xdrpp/message.h"
#include "generated/stellar.hh"
#include "fba/QuorumSet.h"
#include "overlay/StellarMessage.h"

/*
Another peer out there that we are connected to
*/

using namespace std;

namespace stellar
{
    class Application;
    typedef std::shared_ptr<Application> ApplicationPtr;

	class Peer : public enable_shared_from_this <Peer>
	{
	protected:
        ApplicationPtr mApp;
		//boost::asio::io_service::strand mStrand;
		shared_ptr<boost::asio::ip::tcp::socket> mSocket;

		std::mutex mOutputBufferMutex;
		// mOutputBuffer[0] is the message we are currently sending
		deque<StellarMessagePtr> mOutputBuffer;
        vector<uint8_t> mWriteBuffer; //buffer of one message that we keep around while the async_write is happening

		void connectHandler(const boost::system::error_code& ec);
		void reallySendMessage(StellarMessagePtr message);
		void writeHandler(const boost::system::error_code& error, std::size_t bytes_transferred);

        //xdr::msg_ptr mIncomingMsg;

		vector<char> mIncomingHeader;
		vector<uint8_t> mIncomingBody;


        int getIncomingMsgLength();

		void startRead();
		void readHeaderHandler(const boost::system::error_code& error, std::size_t bytes_transferred);
		void readBodyHandler(const boost::system::error_code& error, std::size_t bytes_transferred);

		void recvMessage();

		void recvError(StellarMessagePtr msg);
		void recvHello(StellarMessagePtr msg);
        void recvDontHave(StellarMessagePtr msg);
		void recvGetPeers(StellarMessagePtr msg);
		void recvPeers(StellarMessagePtr msg);
		void recvGetHistory(StellarMessagePtr msg);
		void recvHistory(StellarMessagePtr msg);
		void recvGetDelta(StellarMessagePtr msg);
		void recvDelta(StellarMessagePtr msg);
		void recvGetTxSet(StellarMessagePtr msg);
		void recvTxSet(StellarMessagePtr msg);
		void recvGetValidations(StellarMessagePtr msg);
		void recvValidations(StellarMessagePtr msg);
		void recvTransaction(StellarMessagePtr msg);
		void recvGetQuorumSet(StellarMessagePtr msg);
		void recvQuorumSet(StellarMessagePtr msg);
		void recvFBAMessage(StellarMessagePtr msg);
		

		void sendHello();
        void sendQuorumSet(QuorumSet::pointer qSet);
        void sendDontHave(stellarxdr::MessageType type, stellarxdr::uint256& itemID);
		
		
	public:
		typedef std::shared_ptr<Peer> pointer;

		enum PeerState
		{
			CONNECTING, 
			CONNECTED,
			GOT_HELLO,
			CLOSING
		};

		PeerState mState;
		int mIP;
		int mPort;
		int mRank;

		Peer(ApplicationPtr app,shared_ptr<boost::asio::ip::tcp::socket> socket);
		void createFromDoor();
		void connect();

		
        void sendMessage(StellarMessagePtr msg);
		void sendMessage(stellarxdr::StellarMessage& msg);
		void sendGetTxSet(stellarxdr::uint256& setID);
		void sendGetQuorumSet(stellarxdr::uint256& setID);

		
		static const char *kSQLCreateStatement;
	};
}

#endif

