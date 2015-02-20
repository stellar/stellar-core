// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "overlay/TCPPeer.h"
#include "util/Logging.h"
#include "main/Application.h"
#include "generated/StellarXDR.h"
#include "xdrpp/marshal.h"
#include "overlay/PeerMaster.h"
#include "database/Database.h"
#include "overlay/PeerRecord.h"

#define MS_TO_WAIT_FOR_HELLO 2000

using namespace soci;


namespace stellar
{

using namespace std;

///////////////////////////////////////////////////////////////////////
// TCPPeer
///////////////////////////////////////////////////////////////////////

// make to be called
TCPPeer::TCPPeer(Application& app, std::string& ip, int port)
    : Peer(app, ACCEPTOR), mHelloTimer(app.getClock())
{
    mSocket=make_shared<asio::ip::tcp::socket>(mApp.getMainIOService());
    asio::ip::tcp::endpoint endpoint(asio::ip::address::from_string(ip),
        port);
    mSocket->async_connect(endpoint, std::bind(&Peer::connectHandler, this,
        std::placeholders::_1));
}

// make from door
TCPPeer::TCPPeer(Application& app, shared_ptr<asio::ip::tcp::socket> socket)
    : Peer(app, INITIATOR), mSocket(socket), mHelloTimer(app.getClock())
{
    mHelloTimer.expires_from_now(
        std::chrono::milliseconds(MS_TO_WAIT_FOR_HELLO));
    mHelloTimer.async_wait(std::bind(&TCPPeer::timerExpired, this, std::placeholders::_1));
}

void TCPPeer::timerExpired(const asio::error_code& error)
{
    mSocket->shutdown(asio::socket_base::shutdown_both);
    mSocket->close();
}

void TCPPeer::connected()
{
    mHelloTimer.expires_from_now(
        std::chrono::milliseconds(MS_TO_WAIT_FOR_HELLO));
    mHelloTimer.async_wait(std::bind(&TCPPeer::timerExpired, this, std::placeholders::_1));
}

std::string
TCPPeer::getIP()
{
    return mSocket->remote_endpoint().address().to_string();
}


void
TCPPeer::sendMessage(xdr::msg_ptr&& xdrBytes)
{
    // Pass ownership of a serialized XDR message buffer, along with an
    // asio::buffer pointing into it, to the callback for async_write, so it
    // survives as long as the request is in flight in the io_service, and
    // is deallocated when the write completes.
    //
    // The capture of `buf` is required to keep the buffer alive long enough.

    auto self = shared_from_this();
    auto buf = std::make_shared<xdr::msg_ptr>(std::move(xdrBytes));
    asio::async_write(*(mSocket.get()),
                      asio::buffer((*buf)->raw_data(), (*buf)->raw_size()),
                      [self, buf](asio::error_code const& ec, std::size_t length)
                      {
                          self->writeHandler(ec, length);
                          (void)buf;
                      });
}

void
TCPPeer::writeHandler(const asio::error_code& error,
                      std::size_t bytes_transferred)
{
    if (error)
    {
        CLOG(WARNING, "Overlay") << "writeHandler error: " << error;
        drop();
    }
}

void
TCPPeer::startRead()
{
    auto self = shared_from_this();
    asio::async_read(*(mSocket.get()), asio::buffer(mIncomingHeader),
                     [self](asio::error_code ec, std::size_t length)
                     {
        self->Peer::readHeaderHandler(ec, length);
    });
}

int
TCPPeer::getIncomingMsgLength()
{
    int length = mIncomingHeader[0];
    length <<= 8;
    length |= mIncomingHeader[1];
    length <<= 8;
    length |= mIncomingHeader[2];
    length <<= 8;
    length |= mIncomingHeader[3];
    return (length);
}

void
TCPPeer::readHeaderHandler(const asio::error_code& error,
                           std::size_t bytes_transferred)
{
    if (!error)
    {
        mIncomingBody.resize(getIncomingMsgLength());
        auto self = shared_from_this();
        asio::async_read(*mSocket.get(), asio::buffer(mIncomingBody),
                         [self](asio::error_code ec, std::size_t length)
                         {
            self->Peer::readBodyHandler(ec, length);
        });
    }
    else
    {
        CLOG(WARNING, "Overlay") << "readHeaderHandler error: " << error;
        drop();
    }
}

void
TCPPeer::readBodyHandler(const asio::error_code& error,
                         std::size_t bytes_transferred)
{
    if (!error)
    {
        recvMessage();
        startRead();
    }
    else
    {
        CLOG(WARNING, "Overlay") << "readBodyHandler error: " << error;
        drop();
    }
}

void
TCPPeer::recvMessage()
{
    xdr::xdr_get g(mIncomingBody.data(),
                   mIncomingBody.data() + mIncomingBody.size());
    StellarMessage sm;
    xdr::xdr_argpack_archive(g, sm);
    Peer::recvMessage(sm);
}

void
TCPPeer::recvHello(StellarMessage const& msg)
{
    mHelloTimer.cancel();
    Peer::recvHello(msg);
   
    session& dbSession = mApp.getDatabase().getSession();

    if(mRole==INITIATOR)
    {  
        PeerRecord pr;
        if (!PeerRecord::loadPeerRecord(mApp.getDatabase(), getIP(), getRemoteListeningPort(),  pr))
        {
            PeerRecord::fromIPPort(getIP(), getRemoteListeningPort(), mApp.getClock(), pr);
            pr.storePeerRecord(mApp.getDatabase());
        }

        if(mApp.getPeerMaster().isPeerAccepted(shared_from_this()))
        {
            sendHello();
        }else
        { // we can't accept anymore peer connections
            sendPeers();
            drop();
        }
    } else
    { // we called this guy
        // only lower numFailures if we were successful connecting out to him
        PeerRecord pr;
        PeerRecord::loadPeerRecord(mApp.getDatabase(), getIP(), getRemoteListeningPort(), pr);
        pr.mNumFailures = 0;
        pr.mNextAttempt = mApp.getClock().now();
        pr.storePeerRecord(mApp.getDatabase());
    } 
}



void
TCPPeer::drop()
{
    auto self = shared_from_this();
    auto sock = mSocket;
    mApp.getMainIOService().post(
        [self, sock]()
        {
            self->getApp().getPeerMaster().dropPeer(self);
            sock->shutdown(asio::socket_base::shutdown_both);
            sock->close();
        });
}
}
