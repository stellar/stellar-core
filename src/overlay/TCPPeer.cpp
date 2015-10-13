// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TCPPeer.h"
#include "util/Logging.h"
#include "main/Application.h"
#include "overlay/StellarXDR.h"
#include "xdrpp/marshal.h"
#include "overlay/LoadManager.h"
#include "overlay/OverlayManager.h"
#include "database/Database.h"
#include "overlay/PeerRecord.h"
#include "medida/metrics_registry.h"
#include "medida/meter.h"
#include "main/Config.h"

#define MAX_UNAUTH_MESSAGE_SIZE 0x1000
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
    : Peer(app, role), mSocket(socket)
{
}

TCPPeer::pointer
TCPPeer::initiate(Application& app, std::string const& ip, unsigned short port)
{
    CLOG(DEBUG, "Overlay") << "TCPPeer:initiate"
                           << " to " << ip << ":" << port;
    auto socket =
        make_shared<asio::ip::tcp::socket>(app.getClock().getIOService());
    auto result = make_shared<TCPPeer>(app, WE_CALLED_REMOTE, socket);
    result->mIP = ip;
    result->mRemoteListeningPort = port;
    result->startIdleTimer();
    asio::ip::tcp::endpoint endpoint(asio::ip::address::from_string(ip), port);
    socket->async_connect(
        endpoint, result->mStrand.wrap([result](asio::error_code const& error)
                                       {
                                           result->connectHandler(error);
                                       }));
    return result;
}

TCPPeer::pointer
TCPPeer::accept(Application& app, shared_ptr<asio::ip::tcp::socket> socket)
{
    shared_ptr<TCPPeer> result;
    asio::error_code ec;
    auto ep = socket->remote_endpoint(ec);
    if (!ec)
    {
        CLOG(DEBUG, "Overlay") << "TCPPeer:accept"
            << "@" << app.getConfig().PEER_PORT;
        result = make_shared<TCPPeer>(app, REMOTE_CALLED_US, socket);
        result->mIP = ep.address().to_string();
        result->startIdleTimer();
        result->startRead();
    }
    else
    {
        CLOG(DEBUG, "Overlay") << "TCPPeer:accept"
            << "@" << app.getConfig().PEER_PORT
            << " error " << ec.message();
    }

    return result;
}

TCPPeer::~TCPPeer()
{
    mIdleTimer.cancel();
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

std::string
TCPPeer::getIP()
{
    return mIP;
}

void
TCPPeer::sendMessage(xdr::msg_ptr&& xdrBytes)
{
    CLOG(TRACE, "Overlay") << "TCPPeer:sendMessage to " << toString();

    bool wasEmpty = mWriteQueue.empty();

    // places the buffer to write into the write queue
    auto buf = std::make_shared<xdr::msg_ptr>(std::move(xdrBytes));
    mWriteQueue.emplace(buf);

    if (wasEmpty)
    {
        // kick off the async write chain if we're the first one
        messageSender();
    }
}

void
TCPPeer::messageSender()
{
    // if nothing to do, return
    if (mWriteQueue.empty())
    {
        return;
    }

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    // peek the buffer from the queue
    // do not remove it yet as we need the buffer for the duration of the
    // write operation
    auto buf = mWriteQueue.front();

    asio::async_write(
        *(mSocket.get()), asio::buffer((*buf)->raw_data(), (*buf)->raw_size()),
        mStrand.wrap([self](asio::error_code const& ec, std::size_t length)
                     {
                         self->writeHandler(ec, length);
                         self->mWriteQueue.pop(); // done with front element
                         self->messageSender();   // send the next one
                     }));
}

void
TCPPeer::writeHandler(asio::error_code const& error,
                      std::size_t bytes_transferred)
{
    if (error)
    {
        if (isConnected())
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            mErrorWrite.Mark();
            CLOG(ERROR, "Overlay") << "TCPPeer::writeHandler error to "
                                   << toString();
        }
        drop();
    }
    else
    {
        LoadManager::PeerContext loadCtx(mApp, mPeerID);
        mLastWrite = mApp.getClock().now();
        mMessageWrite.Mark();
        mByteWrite.Mark(bytes_transferred);
    }
}

void
TCPPeer::startRead()
{
    if (shouldAbort())
    {
        return;
    }

    try
    {
        auto self = static_pointer_cast<TCPPeer>(shared_from_this());

        assert(self->mIncomingHeader.size() == 0);
        CLOG(TRACE, "Overlay") << "TCPPeer::startRead to " << self->toString();

        self->mIncomingHeader.resize(4);
        asio::async_read(
            *(self->mSocket.get()), asio::buffer(self->mIncomingHeader),
            self->mStrand.wrap([self](asio::error_code ec, std::size_t length)
                               {
                                   CLOG(TRACE, "Overlay")
                                       << "TCPPeer::startRead calledback " << ec
                                       << " length:" << length;
                                   self->readHeaderHandler(ec, length);
                               }));
    }
    catch (asio::system_error& e)
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
    if (length <= 0 ||
        (!isAuthenticated() && (length > MAX_UNAUTH_MESSAGE_SIZE)) ||
        length > MAX_MESSAGE_SIZE)
    {
        mErrorRead.Mark();
        CLOG(ERROR, "Overlay")
            << "TCP: message size unacceptable: " << length
            << (isAuthenticated() ? "" : " while not authenticated");
        drop();
        length = 0;
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
        LoadManager::PeerContext loadCtx(mApp, mPeerID);
        mByteRead.Mark(bytes_transferred);
        int length = getIncomingMsgLength();
        if (length != 0)
        {
            mIncomingBody.resize(length);
            auto self = static_pointer_cast<TCPPeer>(shared_from_this());
            asio::async_read(
                *mSocket.get(), asio::buffer(mIncomingBody),
                self->mStrand.wrap([self](asio::error_code ec, std::size_t length)
            {
                self->mIncomingHeader.clear();
                self->readBodyHandler(ec, length);
            }));
        }
    }
    else
    {
        if (isConnected())
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            mErrorRead.Mark();
            CLOG(ERROR, "Overlay")
                << "readHeaderHandler error: " << error.message() << " :"
                << toString();
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
        LoadManager::PeerContext loadCtx(mApp, mPeerID);
        mByteRead.Mark(bytes_transferred);
        recvMessage();
        startRead();
    }
    else
    {
        if (isConnected())
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            mErrorRead.Mark();
            CLOG(ERROR, "Overlay")
                << "readBodyHandler error: " << error.message() << " :"
                << toString();
        }
        drop();
    }
}

void
TCPPeer::recvMessage()
{
    mLastRead = mApp.getClock().now();
    mMessageRead.Mark();

    try
    {
        xdr::xdr_get g(mIncomingBody.data(),
                       mIncomingBody.data() + mIncomingBody.size());
        AuthenticatedMessage am;
        xdr::xdr_argpack_archive(g, am);
        Peer::recvMessage(am);
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG(ERROR, "Overlay") << "recvMessage got a corrupt xdr: " << e.what();
        Peer::drop(ERR_DATA, "received corrupt XDR");
    }
}

void
TCPPeer::drop()
{
    if (mState == CLOSING)
    {
        return;
    }

    CLOG(DEBUG, "Overlay") << "TCPPeer::drop " << toString() << " in state "
                           << mState << " we called:" << mRole;

    mState = CLOSING;
    mIdleTimer.cancel();

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());
    getApp().getOverlayManager().dropPeer(self);

    // To shutdown, we first queue up our desire to shutdown in the strand,
    // behind any pending read/write calls. We'll let them issue first.
    mStrand.post(
        [self]()
        {
            // Gracefully shut down connection: this pushes a FIN packet into
            // TCP which, if we wanted to be really polite about, we would wait
            // for an ACK from by doing repeated reads until we get a 0-read.
            //
            // But since we _might_ be dropping a hostile or unresponsive
            // connection, we're going to just post a close() immediately after,
            // and hope the kernel does something useful as far as putting
            // any queued last-gasp ERROR_MSG packet on the wire.
            //
            // All of this is voluntary. We can also just close(2) here and
            // be done with it, but we want to give some chance of telling
            // peers why we're disconnecting them.
            asio::error_code ec;
            self->mSocket->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            if (ec)
            {
                CLOG(ERROR, "Overlay")
                    << "TCPPeer::drop shutdown socket failed: " << ec.message();
            }
            self->mStrand.post(
                [self]()
                {
                    // Close fd associated with socket. Socket is already
                    // shut down, but depending on platform (and apparently
                    // whether there was unread data when we issued
                    // shutdown()) this call might push RST onto the wire,
                    // or some other action; in any case it has to be done
                    // to free the OS resources.
                    //
                    // It will also, at this point, cancel any pending asio
                    // read/write handlers, i.e. fire them with an error
                    // code indicating cancellation.
                    asio::error_code ec2;
                    self->mSocket->close(ec2);
                    if (ec2)
                    {
                        CLOG(ERROR, "Overlay")
                            << "TCPPeer::drop close socket failed: "
                            << ec2.message();
                    }
                });
        });
}
}
