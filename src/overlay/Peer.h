#ifndef __PEER__
#define __PEER__

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>
#include <deque>
#include <mutex>
#include "xdrpp/message.h"
#include "generated/stellar.hh"
#include "fba/QuorumSet.h"
#include "overlay/StellarMessage.h"
#include "util/timer.h"

/*
Another peer out there that we are connected to
*/

using namespace std;

namespace stellar
{
    class Application;

	class Peer : public enable_shared_from_this <Peer>
	{
	protected:
        Application &mApp;
		shared_ptr<asio::ip::tcp::socket> mSocket;

		void connectHandler(const asio::error_code& ec);
		void writeHandler(const asio::error_code& error, std::size_t bytes_transferred);

		uint8_t mIncomingHeader[4];
		vector<uint8_t> mIncomingBody;
        Timer mHelloTimer;

        int getIncomingMsgLength();

		void startRead();
		void readHeaderHandler(const asio::error_code& error, std::size_t bytes_transferred);
		void readBodyHandler(const asio::error_code& error, std::size_t bytes_transferred);

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
        void sendPeers();
		
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
		std::string mIP;
		int mPort; // the port they accept connections on
		int mRank;
        std::string mVersion;
        int mProtocolVersion;

        Peer(Application &app, shared_ptr<asio::ip::tcp::socket> socket);
		void createFromDoor();
		void connect();
        void drop();
		
		void sendMessage(stellarxdr::StellarMessage& msg);
		void sendGetTxSet(stellarxdr::uint256& setID);
		void sendGetQuorumSet(stellarxdr::uint256& setID);

		
		static const char *kSQLCreateStatement;
	};
}

#endif

