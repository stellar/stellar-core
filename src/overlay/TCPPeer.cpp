// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "overlay/TCPPeer.h"
#include "util/Logging.h"
#include "main/Application.h"
#include "generated/StellarXDR.h"
#include "xdrpp/marshal.h"
#include "overlay/PeerMaster.h"

#define MS_TO_WAIT_FOR_HELLO 2000

namespace stellar
{
///////////////////////////////////////////////////////////////////////
// TCPPeer
///////////////////////////////////////////////////////////////////////

const char* TCPPeer::kSQLCreateStatement =
    "CREATE TABLE IF NOT EXISTS Peers (                      \
        peerID      INT PRIMARY KEY AUTO_INCREMENT, \
        ip          varchar(16),            \
        port        INT,                \
        lastTry     timestamp,          \
        lastConnect timestamp,      \
        rank    INT     \
    );";

TCPPeer::TCPPeer(Application& app, shared_ptr<asio::ip::tcp::socket> socket,
                 PeerRole role)
    : Peer(app, role), mSocket(socket), mHelloTimer(app.getMainIOService())
{
    mHelloTimer.expires_from_now(
        std::chrono::milliseconds(MS_TO_WAIT_FOR_HELLO));
    mHelloTimer.async_wait(
        [socket](asio::error_code const& ec)
        {
            socket->shutdown(asio::socket_base::shutdown_both);
            socket->close();
        });
}

std::string
TCPPeer::getIP()
{
    return mSocket->remote_endpoint().address().to_string();
}

void
TCPPeer::connect()
{
    // GRAYDON mSocket->async_connect(server_endpoint, your_completion_handler);
}

void
TCPPeer::sendMessage(xdr::msg_ptr&& xdrBytes)
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
                      asio::buffer(xdrBytes->raw_data(), xdrBytes->raw_size()),
                      std::bind(
                          [self](asio::error_code const& ec, std::size_t length,
                                 xdr::msg_ptr const&)
                          {
                              self->writeHandler(ec, length);
                          },
                          _1, _2, std::move(xdrBytes)));
}

void
TCPPeer::writeHandler(const asio::error_code& error,
                      std::size_t bytes_transferred)
{
    if (error)
    {
        CLOG(WARNING, "Overlay") << "writeHandler error: " << error;
        // LATER drop Peer
    }
}

void
TCPPeer::startRead()
{
    auto self = shared_from_this();
    asio::async_read(*(mSocket.get()), asio::buffer(mIncomingHeader),
                     [self](std::error_code ec, std::size_t length)
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
                         [self](std::error_code ec, std::size_t length)
                         {
            self->Peer::readBodyHandler(ec, length);
        });
    }
    else
    {
        CLOG(WARNING, "Overlay") << "readHeaderHandler error: " << error;
        // LATER drop Peer
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
        // LATER drop Peer
    }
}

void
TCPPeer::recvMessage()
{
    xdr::xdr_get g(mIncomingBody.data(),
                   mIncomingBody.data() + mIncomingBody.size());
    stellarxdr::StellarMessage sm;
    xdr::xdr_argpack_archive(g, sm);
    Peer::recvMessage(sm);
}

void
TCPPeer::recvHello(stellarxdr::StellarMessage const& msg)
{
    mHelloTimer.cancel();
    Peer::recvHello(msg);
    if (!mApp.getPeerMaster().isPeerAccepted(shared_from_this()))
    { // we can't accept anymore peer connections
        sendPeers();
        drop();
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
