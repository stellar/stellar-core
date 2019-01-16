// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TCPPeer.h"
#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "overlay/LoadManager.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerRecord.h"
#include "overlay/StellarXDR.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"

using namespace soci;

namespace stellar
{

using namespace std;

///////////////////////////////////////////////////////////////////////
// TCPPeer
///////////////////////////////////////////////////////////////////////

TCPPeer::TCPPeer(Application& app, Peer::PeerRole role,
                 std::shared_ptr<TCPPeer::SocketType> socket)
    : Peer(app, role), mSocket(socket)
{
}

TCPPeer::pointer
TCPPeer::initiate(Application& app, PeerBareAddress const& address)
{
    assert(address.getType() == PeerBareAddress::Type::IPv4);

    CLOG(DEBUG, "Overlay") << "TCPPeer:initiate"
                           << " to " << address.toString();
    assertThreadIsMain();
    auto socket = make_shared<SocketType>(app.getClock().getIOService());
    auto result = make_shared<TCPPeer>(app, WE_CALLED_REMOTE, socket);
    result->mAddress = address;
    result->startIdleTimer();
    asio::ip::tcp::endpoint endpoint(
        asio::ip::address::from_string(address.getIP()), address.getPort());
    socket->next_layer().async_connect(
        endpoint, [result](asio::error_code const& error) {
            asio::error_code ec;
            if (!error)
            {
                asio::ip::tcp::no_delay nodelay(true);
                result->mSocket->next_layer().set_option(nodelay, ec);
            }
            else
            {
                ec = error;
            }

            result->connectHandler(ec);
        });
    return result;
}

TCPPeer::pointer
TCPPeer::accept(Application& app, shared_ptr<TCPPeer::SocketType> socket)
{
    assertThreadIsMain();
    shared_ptr<TCPPeer> result;
    asio::error_code ec;

    asio::ip::tcp::no_delay nodelay(true);
    socket->next_layer().set_option(nodelay, ec);

    if (!ec)
    {
        CLOG(DEBUG, "Overlay") << "TCPPeer:accept"
                               << "@" << app.getConfig().PEER_PORT;
        result = make_shared<TCPPeer>(app, REMOTE_CALLED_US, socket);
        result->startIdleTimer();
        result->startRead();
    }
    else
    {
        CLOG(DEBUG, "Overlay")
            << "TCPPeer:accept"
            << "@" << app.getConfig().PEER_PORT << " error " << ec.message();
    }

    return result;
}

TCPPeer::~TCPPeer()
{
    assertThreadIsMain();
    mIdleTimer.cancel();
    if (mSocket)
    {
        // Ignore: this indicates an attempt to cancel events
        // on a not-established socket.
        asio::error_code ec;

#ifndef _WIN32
        // This always fails on windows and ASIO won't
        // even build it.
        mSocket->next_layer().cancel(ec);
#endif
        mSocket->close(ec);
    }
}

PeerBareAddress
TCPPeer::makeAddress(int remoteListeningPort) const
{
    asio::error_code ec;
    auto ep = mSocket->next_layer().remote_endpoint(ec);
    if (ec || remoteListeningPort <= 0 || remoteListeningPort > UINT16_MAX)
    {
        return PeerBareAddress{};
    }
    else
    {
        return PeerBareAddress{
            ep.address().to_string(),
            static_cast<unsigned short>(remoteListeningPort)};
    }
}

void
TCPPeer::sendMessage(xdr::msg_ptr&& xdrBytes)
{
    if (mState == CLOSING)
    {
        CLOG(ERROR, "Overlay")
            << "Trying to send message to " << toString() << " after drop";
        return;
    }

    if (Logging::logTrace("Overlay"))
        CLOG(TRACE, "Overlay") << "TCPPeer:sendMessage to " << toString();
    assertThreadIsMain();

    // places the buffer to write into the write queue
    auto buf = std::make_shared<xdr::msg_ptr>(std::move(xdrBytes));

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    self->mWriteQueue.emplace(buf);

    if (!self->mWriting)
    {
        self->mWriting = true;
        // kick off the async write chain if we're the first one
        self->messageSender();
    }
}

void
TCPPeer::shutdown()
{
    if (mShutdownScheduled)
    {
        // should not happen, leave here for debugging purposes
        CLOG(ERROR, "Overlay") << "Double schedule of shutdown " << toString();
        return;
    }

    mIdleTimer.cancel();
    mShutdownScheduled = true;
    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    // To shutdown, we first queue up our desire to shutdown in the strand,
    // behind any pending read/write calls. We'll let them issue first.
    self->getApp().postOnMainThread(
        [self]() {
            // Gracefully shut down connection: this pushes a FIN packet into
            // TCP which, if we wanted to be really polite about, we would wait
            // for an ACK from by doing repeated reads until we get a 0-read.
            //
            // But since we _might_ be dropping a hostile or unresponsive
            // connection, we're going to just post a close() immediately after,
            // and hope the kernel does something useful as far as putting any
            // queued last-gasp ERROR_MSG packet on the wire.
            //
            // All of this is voluntary. We can also just close(2) here and be
            // done with it, but we want to give some chance of telling peers
            // why we're disconnecting them.
            asio::error_code ec;
            self->mSocket->next_layer().shutdown(
                asio::ip::tcp::socket::shutdown_both, ec);
            if (ec)
            {
                CLOG(ERROR, "Overlay")
                    << "TCPPeer::drop shutdown socket failed: " << ec.message();
            }
            self->getApp().postOnMainThread(
                [self]() {
                    // Close fd associated with socket. Socket is already shut
                    // down, but depending on platform (and apparently whether
                    // there was unread data when we issued shutdown()) this
                    // call might push RST onto the wire, or some other action;
                    // in any case it has to be done to free the OS resources.
                    //
                    // It will also, at this point, cancel any pending asio
                    // read/write handlers, i.e. fire them with an error code
                    // indicating cancellation.
                    asio::error_code ec2;
                    self->mSocket->close(ec2);
                    if (ec2)
                    {
                        CLOG(ERROR, "Overlay")
                            << "TCPPeer::drop close socket failed: "
                            << ec2.message();
                    }
                },
                "TCPPeer: close");
        },
        "TCPPeer: shutdown");
}

void
TCPPeer::messageSender()
{
    assertThreadIsMain();

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    // if nothing to do, flush and return
    if (mWriteQueue.empty())
    {
        mSocket->async_flush([self](asio::error_code const& ec, std::size_t) {
            self->writeHandler(ec, 0);
            if (!ec)
            {
                if (!self->mWriteQueue.empty())
                {
                    self->messageSender();
                }
                else
                {
                    self->mWriting = false;
                    // there is nothing to send and delayed shutdown was
                    // requested - time to perform it
                    if (self->mDelayedShutdown)
                    {
                        self->shutdown();
                    }
                }
            }
        });
        return;
    }

    // peek the buffer from the queue
    // do not remove it yet as we need the buffer for the duration of the
    // write operation
    auto buf = mWriteQueue.front();

    asio::async_write(*(mSocket.get()),
                      asio::buffer((*buf)->raw_data(), (*buf)->raw_size()),
                      [self](asio::error_code const& ec, std::size_t length) {
                          self->writeHandler(ec, length);
                          self->mWriteQueue.pop(); // done with front element

                          // continue processing the queue/flush
                          if (!ec)
                          {
                              self->messageSender();
                          }
                      });
}

void
TCPPeer::writeHandler(asio::error_code const& error,
                      std::size_t bytes_transferred)
{
    assertThreadIsMain();
    mLastWrite = mApp.getClock().now();

    if (error)
    {
        if (isConnected())
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            mErrorWrite.Mark();
            CLOG(ERROR, "Overlay")
                << "TCPPeer::writeHandler error to " << toString();
        }
        if (mDelayedShutdown)
        {
            // delayed shutdown was requested - time to perform it
            shutdown();
        }
        else
        {
            // no delayed shutdown - we can drop normally
            drop();
        }
    }
    else if (bytes_transferred != 0)
    {
        LoadManager::PeerContext loadCtx(mApp, mPeerID);
        mMessageWrite.Mark();
        mByteWrite.Mark(bytes_transferred);
    }
}

void
TCPPeer::startRead()
{
    assertThreadIsMain();
    if (shouldAbort())
    {
        return;
    }

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    assert(self->mIncomingHeader.size() == 0);

    if (Logging::logTrace("Overlay"))
        CLOG(TRACE, "Overlay") << "TCPPeer::startRead to " << self->toString();

    self->mIncomingHeader.resize(4);
    asio::async_read(*(self->mSocket.get()),
                     asio::buffer(self->mIncomingHeader),
                     [self](asio::error_code ec, std::size_t length) {
                         if (Logging::logTrace("Overlay"))
                             CLOG(TRACE, "Overlay")
                                 << "TCPPeer::startRead calledback " << ec
                                 << " length:" << length;
                         self->readHeaderHandler(ec, length);
                     });
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
    assertThreadIsMain();
    // LOG(DEBUG) << "TCPPeer::readHeaderHandler "
    //     << "@" << mApp.getConfig().PEER_PORT
    //     << " to " << mRemoteListeningPort
    //     << (error ? "error " : "") << " bytes:" << bytes_transferred;

    if (!error)
    {
        receivedBytes(bytes_transferred, false);
        int length = getIncomingMsgLength();
        if (length != 0)
        {
            mIncomingBody.resize(length);
            auto self = static_pointer_cast<TCPPeer>(shared_from_this());
            asio::async_read(*mSocket.get(), asio::buffer(mIncomingBody),
                             [self](asio::error_code ec, std::size_t length) {
                                 self->readBodyHandler(ec, length);
                             });
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
    assertThreadIsMain();
    // LOG(DEBUG) << "TCPPeer::readBodyHandler "
    //     << "@" << mApp.getConfig().PEER_PORT
    //     << " to " << mRemoteListeningPort
    //     << (error ? "error " : "") << " bytes:" << bytes_transferred;

    if (!error)
    {
        receivedBytes(bytes_transferred, true);
        recvMessage();
        mIncomingHeader.clear();
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
    assertThreadIsMain();
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
TCPPeer::drop(bool force)
{
    assertThreadIsMain();
    if (shouldAbort())
    {
        return;
    }

    CLOG(DEBUG, "Overlay") << "TCPPeer::drop " << toString() << " in state "
                           << mState << " we called:" << mRole;

    mState = CLOSING;

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());
    getApp().getOverlayManager().dropPeer(this);

    // if write queue is not empty, messageSender will take care of shutdown
    if (force || !mWriting)
    {
        self->shutdown();
    }
    else
    {
        self->mDelayedShutdown = true;
    }
}
}
