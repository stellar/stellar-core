// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TCPPeer.h"
#include "util/Logging.h"
#include "main/Application.h"
#include "generated/StellarXDR.h"
#include "xdrpp/marshal.h"
#include "overlay/OverlayManager.h"
#include "database/Database.h"
#include "overlay/PeerRecord.h"
#include "medida/metrics_registry.h"
#include "medida/meter.h"
#include "main/Config.h"

#define IO_TIMEOUT_SECONDS 30
#define MAX_MESSAGE_SIZE 0x1000000

using namespace soci;

namespace stellar
{

using namespace std;

///////////////////////////////////////////////////////////////////////
// TCPPeer
///////////////////////////////////////////////////////////////////////

TCPPeer::TCPPeer(Application& app, Peer::PeerRole role,
                 std::shared_ptr<asio::ip::tcp::socket> socket)
    : Peer(app, role)
    , mSocket(socket)
    , mReadIdle(app)
    , mWriteIdle(app)
    , mMessageRead(
          app.getMetrics().NewMeter({"overlay", "message", "read"}, "message"))
    , mMessageWrite(
          app.getMetrics().NewMeter({"overlay", "message", "write"}, "message"))
    , mByteRead(app.getMetrics().NewMeter({"overlay", "byte", "read"}, "byte"))
    , mByteWrite(
          app.getMetrics().NewMeter({"overlay", "byte", "write"}, "byte"))
    , mErrorRead(
          app.getMetrics().NewMeter({"overlay", "error", "read"}, "error"))
    , mErrorWrite(
          app.getMetrics().NewMeter({"overlay", "error", "write"}, "error"))
    , mTimeoutRead(
          app.getMetrics().NewMeter({"overlay", "timeout", "read"}, "timeout"))
    , mTimeoutWrite(
          app.getMetrics().NewMeter({"overlay", "timeout", "write"}, "timeout"))
{
}

TCPPeer::pointer
TCPPeer::initiate(Application& app, std::string const& ip, unsigned short port)
{
    CLOG(DEBUG, "Overlay") << "TCPPeer:initiate"
                           << "@" << app.getConfig().PEER_PORT << " to " << ip
                           << ":" << port;
    auto socket =
        make_shared<asio::ip::tcp::socket>(app.getClock().getIOService());
    auto result = make_shared<TCPPeer>(
        app, ACCEPTOR,
        socket); // We are initiating; new `newed` TCPPeer is accepting
    result->mIP = ip;
    result->mRemoteListeningPort = port;
    asio::ip::tcp::endpoint endpoint(asio::ip::address::from_string(ip), port);
    socket->async_connect(endpoint, [result](asio::error_code const& error)
                          {
                              result->connectHandler(error);
                          });
    return result;
}

TCPPeer::pointer
TCPPeer::accept(Application& app, shared_ptr<asio::ip::tcp::socket> socket)
{
    CLOG(DEBUG, "Overlay") << "TCPPeer:accept"
                           << "@" << app.getConfig().PEER_PORT;
    auto result = make_shared<TCPPeer>(
        app, INITIATOR,
        socket); // We are accepting; new `newed` TCPPeer initiated
    result->mIP = socket->remote_endpoint().address().to_string();
    result->startRead();
    return result;
}

TCPPeer::~TCPPeer()
{
    mWriteIdle.cancel();
    mReadIdle.cancel();
    try
    {
        if (mSocket)
        {
#ifndef _WIN32
            // This always fails on windows and ASIO won't
            // even build it.
            mSocket->cancel();
#endif
            mSocket->close();
        }
    }
    catch (asio::system_error&)
    {
        // Ignore: this indicates an attempt to cancel events
        // on a not-established socket.
    }
}

void
TCPPeer::resetWriteIdle()
{
    std::shared_ptr<TCPPeer> self =
        std::dynamic_pointer_cast<TCPPeer>(shared_from_this());
    mWriteIdle.expires_from_now(std::chrono::seconds(IO_TIMEOUT_SECONDS));
    mWriteIdle.async_wait([self](const asio::error_code& error)
                          {
                              if (!error)
                                  self->timeoutWrite(error);
                          });
}

void
TCPPeer::resetReadIdle()
{
    std::shared_ptr<TCPPeer> self =
        std::dynamic_pointer_cast<TCPPeer>(shared_from_this());
    mReadIdle.expires_from_now(std::chrono::seconds(IO_TIMEOUT_SECONDS));
    mReadIdle.async_wait([self](const asio::error_code& error)
                         {
                             if (!error)
                                 self->timeoutRead(error);
                         });
}

void
TCPPeer::timeoutWrite(asio::error_code const& error)
{
    CLOG(DEBUG, "Overlay") << "write timeout";
    mTimeoutWrite.Mark();
    drop();
}

void
TCPPeer::timeoutRead(asio::error_code const& error)
{
    CLOG(INFO, "Overlay") << "read timeout";
    mTimeoutRead.Mark();
    drop();
}

std::string
TCPPeer::getIP()
{
    return mIP;
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

    CLOG(DEBUG, "Overlay") << "TCPPeer:sendMessage"
                           << "@" << mApp.getConfig().PEER_PORT << " to "
                           << mRemoteListeningPort;

    resetWriteIdle();
    auto self = shared_from_this();
    auto buf = std::make_shared<xdr::msg_ptr>(std::move(xdrBytes));
    asio::async_write(
        *(mSocket.get()), asio::buffer((*buf)->raw_data(), (*buf)->raw_size()),
        [self, buf](asio::error_code const& ec, std::size_t length)
        {
            self->writeHandler(ec, length);
            (void)buf;
        });
}

void
TCPPeer::writeHandler(asio::error_code const& error,
                      std::size_t bytes_transferred)
{
    if (error)
    {
        if (mState == CONNECTED || mState == GOT_HELLO)
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            mErrorWrite.Mark();
            CLOG(ERROR, "Overlay") << "TCPPeer::writeHandler error"
                                   << "@" << mApp.getConfig().PEER_PORT
                                   << " to " << mRemoteListeningPort;
        }
        drop();
    }
    else
    {
        mMessageWrite.Mark();
        mByteWrite.Mark(bytes_transferred);
    }
}

void
TCPPeer::startRead()
{
    try
    {
        CLOG(DEBUG, "Overlay") << "TCPPeer::startRead"
            << "@" << mApp.getConfig().PEER_PORT << " to "
            << mSocket->remote_endpoint().port();
        resetReadIdle();
        auto self = shared_from_this();
        asio::async_read(*(mSocket.get()), asio::buffer(mIncomingHeader),
            [self](asio::error_code ec, std::size_t length)
        {
            CLOG(DEBUG, "Overlay")
                << "TCPPeer::startRead calledback " << ec
                << " length:" << length;
            self->readHeaderHandler(ec, length);
        });
    }
    catch (asio::system_error &e)
    {
        CLOG(ERROR, "Overlay") << "TCPPeer::startRead error " << e.what();
        drop();
    }
}

int
TCPPeer::getIncomingMsgLength()
{
    int length = mIncomingHeader[0];
    length &= 0x7f; // clear the XDR 'continuation' bit
    length <<= 8;
    length |= mIncomingHeader[1];
    length <<= 8;
    length |= mIncomingHeader[2];
    length <<= 8;
    length |= mIncomingHeader[3];
    if (length < 0 || length > MAX_MESSAGE_SIZE)
    {
        mErrorRead.Mark();
        CLOG(ERROR, "Overlay")
            << "TCP::Peer::getIncomingMsgLength message size unacceptable: "
            << length;
        drop();
    }
    return (length);
}

void
TCPPeer::connected()
{
    startRead();
}

void
TCPPeer::readHeaderHandler(asio::error_code const& error,
                           std::size_t bytes_transferred)
{
    // LOG(DEBUG) << "TCPPeer::readHeaderHandler "
    //     << "@" << mApp.getConfig().PEER_PORT
    //     << " to " << mRemoteListeningPort
    //     << (error ? "error " : "") << " bytes:" << bytes_transferred;

    if (!error)
    {
        mByteRead.Mark(bytes_transferred);
        mIncomingBody.resize(getIncomingMsgLength());
        auto self = shared_from_this();
        asio::async_read(*mSocket.get(), asio::buffer(mIncomingBody),
                         [self](asio::error_code ec, std::size_t length)
                         {
                             self->readBodyHandler(ec, length);
                         });
    }
    else
    {
        if (mState == CONNECTED || mState == GOT_HELLO)
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            mErrorRead.Mark();
            CLOG(ERROR, "Overlay")
                << "readHeaderHandler error: " << error.message();
        }
        drop();
    }
}

void
TCPPeer::readBodyHandler(asio::error_code const& error,
                         std::size_t bytes_transferred)
{
    // LOG(DEBUG) << "TCPPeer::readBodyHandler "
    //     << "@" << mApp.getConfig().PEER_PORT
    //     << " to " << mRemoteListeningPort
    //     << (error ? "error " : "") << " bytes:" << bytes_transferred;

    if (!error)
    {
        mByteRead.Mark(bytes_transferred);
        recvMessage();
        startRead();
    }
    else
    {
        if (mState == CONNECTED || mState == GOT_HELLO)
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            mErrorRead.Mark();
            CLOG(ERROR, "Overlay")
                << "readBodyHandler error: " << error.message();
        }
        drop();
    }
}

void
TCPPeer::recvMessage()
{
    xdr::xdr_get g(mIncomingBody.data(),
                   mIncomingBody.data() + mIncomingBody.size());
    mMessageRead.Mark();
    StellarMessage sm;
    xdr::xdr_argpack_archive(g, sm);
    Peer::recvMessage(sm);
}

bool
TCPPeer::recvHello(StellarMessage const& msg)
{
    if (!Peer::recvHello(msg))
        return false;

    if (mRole == INITIATOR)
    {
        if (!PeerRecord::loadPeerRecord(mApp.getDatabase(), getIP(),
                                        getRemoteListeningPort()))
        {
            PeerRecord pr;
            PeerRecord::fromIPPort(getIP(), getRemoteListeningPort(),
                                   mApp.getClock(), pr);
            pr.storePeerRecord(mApp.getDatabase());
        }

        if (mApp.getOverlayManager().isPeerAccepted(shared_from_this()))
        {
            sendHello();
            sendPeers();
        }
        else
        { // we can't accept anymore peer connections
            sendPeers();
            drop();
        }
    }
    else
    { // we called this guy
        // only lower numFailures if we were successful connecting out to him
        auto pr = PeerRecord::loadPeerRecord(mApp.getDatabase(), getIP(),
                                             getRemoteListeningPort());
        if (!pr)
        {
            pr = make_optional<PeerRecord>();
            PeerRecord::fromIPPort(getIP(), getRemoteListeningPort(),
                                   mApp.getClock(), *pr);
        }
        pr->mNumFailures = 0;
        pr->mNextAttempt = mApp.getClock().now();
        pr->storePeerRecord(mApp.getDatabase());
    }
    return true;
}

void
TCPPeer::drop()
{
    if (mState == CLOSING)
    {
        return;
    }

    bool wasConnected = (mState == CONNECTED || mState == GOT_HELLO);

    CLOG(INFO, "Overlay") << "TCPPeer::drop"
                          << "@" << mApp.getConfig().PEER_PORT << " to "
                          << mRemoteListeningPort << " in state " << mState;

    mState = CLOSING;

    CLOG(DEBUG, "Overlay") << "TCPPeer:drop"
                           << "@" << mApp.getConfig().PEER_PORT << " to "
                           << mRemoteListeningPort;

    mWriteIdle.cancel();
    mReadIdle.cancel();
    auto self = shared_from_this();
    auto sock = mSocket;

    // We post the shutdown to io_service so that any final writes have a chance
    // to get ahead of the shutdown and actually make it onto the wire.
    mApp.getClock().getIOService().post(
        [self, sock, wasConnected]()
        {
            self->getApp().getOverlayManager().dropPeer(self);
            if (wasConnected)
            {
                try
                {
                    sock->shutdown(asio::socket_base::shutdown_both);
                }
                catch (asio::system_error& e)
                {
                    CLOG(ERROR, "Overlay")
                        << "TCPPeer::drop shutdown failed: " << e.what();
                }
                catch (...)
                {
                    CLOG(ERROR, "Overlay")
                        << "TCPPeer::drop socket shutdown failed";
                }
            }
            sock->close();
        });
}
}
