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

namespace stellar
{
    using namespace std;

    class Application;
    class LoopbackPeer;

    class Peer : public enable_shared_from_this <Peer>
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
        Application &mApp;

        PeerRole mRole;
        PeerState mState;

        std::string mRemoteVersion;
        int mRemoteProtocolVersion;
        int mRemoteListeningPort;
        void recvMessage(StellarMessagePtr msg);
        void recvMessage(xdr::msg_ptr const& xdrBytes);

        virtual void recvError(StellarMessagePtr msg);
        virtual void recvHello(StellarMessagePtr msg);
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

        virtual void sendMessage(xdr::msg_ptr&& xdrBytes) = 0;

    public:

        Peer(Application &app, PeerRole role);
        Application &getApp() { return mApp; }

        void sendGetTxSet(stellarxdr::uint256& setID);
        void sendGetQuorumSet(stellarxdr::uint256& setID);

        void sendMessage(stellarxdr::StellarMessage msg);

        PeerRole getRole() const { return mRole; }
        PeerState getState() const { return mState; }

        std::string const &getRemoreVersion() const { return mRemoteVersion; }
        int getRemoteProtocolVersion() const { return mRemoteProtocolVersion; }
        int getRemoteListeningPort() { return mRemoteListeningPort; }

        // These exist mostly to be overridden in TCPPeer and callable via
        // shared_ptr<Peer> as a captured shared_from_this().
        virtual void connectHandler(const asio::error_code& ec);
        virtual void writeHandler(const asio::error_code& error, std::size_t bytes_transferred) {}
        virtual void readHeaderHandler(const asio::error_code& error, std::size_t bytes_transferred) {}
        virtual void readBodyHandler(const asio::error_code& error, std::size_t bytes_transferred) {}

        virtual void drop() = 0;
        virtual std::string getIP() = 0;
        virtual ~Peer() {}

        friend class LoopbackPeer;
    };


    // Peer that communicates via a TCP socket.
    class TCPPeer : public Peer
    {
        shared_ptr<asio::ip::tcp::socket> mSocket;
        Timer mHelloTimer;
        uint8_t mIncomingHeader[4];
        vector<uint8_t> mIncomingBody;

        void connect();
        void recvMessage();
        void recvHello(StellarMessagePtr msg);
        void sendMessage(xdr::msg_ptr&& xdrBytes);
        int getIncomingMsgLength();
        void startRead();

        void writeHandler(const asio::error_code& error, std::size_t bytes_transferred);
        void readHeaderHandler(const asio::error_code& error, std::size_t bytes_transferred);
        void readBodyHandler(const asio::error_code& error, std::size_t bytes_transferred);

        static const char *kSQLCreateStatement;

    public:

        TCPPeer(Application &app, shared_ptr<asio::ip::tcp::socket> socket, PeerRole role);
        virtual ~TCPPeer() {}


        void drop();
        std::string getIP();
    };


    // [testing] Peer that communicates via byte-buffer delivery events queued in
    // in-process io_services.
    //
    // NB: Do not construct one of these directly; instead, construct a connected
    // pair of them wrapped in a LoopbackPeerConnection that explicitly manages the
    // lifecycle of the connection.

    class LoopbackPeer : public Peer
    {
      private:
        std::shared_ptr<LoopbackPeer> mRemote;
        std::deque<xdr::msg_ptr> mQueue;

        bool mCorked{false};
        size_t mMaxQueueDepth{0};

        std::default_random_engine mGenerator;
        std::bernoulli_distribution mDuplicateProb{0.0};
        std::bernoulli_distribution mReorderProb{0.0};
        std::bernoulli_distribution mDamageProb{0.0};
        std::bernoulli_distribution mDropProb{0.0};

        struct Stats
        {
            size_t messagesDuplicated{0};
            size_t messagesReordered{0};
            size_t messagesDamaged{0};
            size_t messagesDropped{0};

            size_t bytesDelivered{0};
            size_t messagesDelivered{0};
        };

        Stats mStats;

        void sendMessage(xdr::msg_ptr &&xdrBytes);

      public:
        virtual ~LoopbackPeer()
        {
        }
        LoopbackPeer(Application &app, PeerRole role);
        void drop();
        std::string getIP();

        void deliverOne();
        void deliverAll();
        void dropAll();
        size_t getBytesQueued() const;
        size_t getMessagesQueued() const;

        Stats const &getStats() const;
        std::deque<xdr::msg_ptr> &getQueue();
        std::shared_ptr<LoopbackPeer> const &getTarget() const;

        bool getCorked() const;
        void setCorked(bool c);

        int getMaxQueueDepth() const;
        void setMaxQueueDepth(size_t sz);

        double getDamageProbability() const;
        void setDamageProbability(double d);

        double getDropProbability() const;
        void setDropProbability(double d);

        double getDuplicateProbability() const;
        void setDuplicateProbability(double d);

        double getReorderProbability() const;
        void setReorderProbability(double d);

        friend class LoopbackPeerConnection;
    };

    /**
     * Testing class for managing a simulated network connection between two
     * LoopbackPeers.
     */
    class LoopbackPeerConnection
    {
        shared_ptr<LoopbackPeer> mInitiator;
        shared_ptr<LoopbackPeer> mAcceptor;

      public:
        LoopbackPeerConnection(Application &initiator, Application &acceptor);
        ~LoopbackPeerConnection();
        shared_ptr<LoopbackPeer> const &getInitiator() const;
        shared_ptr<LoopbackPeer> const &getAcceptor() const;
    };
}

#endif

