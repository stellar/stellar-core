// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TCPPeer.h"
#include "crypto/CryptoError.h"
#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "medida/meter.h"
#include "overlay/FlowControl.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "util/GlobalChecks.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>
#include <fmt/format.h>

using namespace soci;

namespace stellar
{

using namespace std;

///////////////////////////////////////////////////////////////////////
// TCPPeer
///////////////////////////////////////////////////////////////////////

TCPPeer::TCPPeer(Application& app, Peer::PeerRole role,
                 std::shared_ptr<TCPPeer::SocketType> socket,
                 std::string address)
    : Peer(app, role)
    , mThreadVars(useBackgroundThread())
    , mSocket(socket)
    , mIPAddress(std::move(address))
    , mLiveInboundPeersCounter(
          app.getOverlayManager().getLiveInboundPeersCounter())
{
    releaseAssert(threadIsMain());
    if (mRole == REMOTE_CALLED_US)
    {
        (*mLiveInboundPeersCounter)++;
    }
}

TCPPeer::pointer
TCPPeer::initiate(Application& app, PeerBareAddress const& address)
{
    releaseAssert(threadIsMain());
    releaseAssert(address.getType() == PeerBareAddress::Type::IPv4);

    CLOG_DEBUG(Overlay, "TCPPeer:initiate to {}", address.toString());
    auto& ioContext = app.getConfig().BACKGROUND_OVERLAY_PROCESSING
                          ? app.getOverlayIOContext()
                          : app.getClock().getIOContext();
    auto socket = make_shared<SocketType>(ioContext, BUFSZ);
    auto result =
        make_shared<TCPPeer>(app, WE_CALLED_REMOTE, socket, address.toString());
    result->initialize(address);
    asio::ip::tcp::endpoint endpoint(
        asio::ip::address::from_string(address.getIP()), address.getPort());
    // Use weak_ptr here in case the peer is dropped before main thread saves
    // strong reference in pending/authenticated peer lists
    socket->next_layer().async_connect(
        endpoint, [weak = std::weak_ptr<TCPPeer>(result),
                   address](asio::error_code const& error) {
            auto result = weak.lock();
            if (!result)
            {
                // If peer was rejected for reasons like no pending slots
                // available, and isn't referenced by the main thread anymore,
                // exit early.
                return;
            }

            releaseAssert(!threadIsMain() || !result->useBackgroundThread());

            // We might have been dropped while waiting to connect; in this
            // case, do not execute handler and just exit
            RECURSIVE_LOCK_GUARD(result->mStateMutex, guard);
            if (result->shouldAbort(guard))
            {
                return;
            }
            asio::error_code ec;
            asio::error_code lingerEc;
            if (!error)
            {
                asio::ip::tcp::no_delay nodelay(true);
                asio::ip::tcp::socket::linger linger(false, 0);
                std::ignore =
                    result->mSocket->next_layer().set_option(nodelay, ec);
                std::ignore =
                    result->mSocket->next_layer().set_option(linger, lingerEc);
            }
            else
            {
                ec = error;
            }

            auto finalEc = ec ? ec : lingerEc;
            result->connectHandler(finalEc);
        });
    return result;
}

TCPPeer::pointer
TCPPeer::accept(Application& app, shared_ptr<TCPPeer::SocketType> socket)
{
    releaseAssert(threadIsMain());

    // Asio guarantees it is safe to create a socket object with overlay's
    // io_context on main (as long as the socket object isn't shared across
    // threads) Therefore, it is safe to call functions like `remote_endpoint`
    // and `set_option` (the socket object isn't passed to background yet)
    auto extractIP = [](shared_ptr<SocketType> socket) {
        std::string result;
        asio::error_code ec;
        auto ep = socket->next_layer().remote_endpoint(ec);
        if (ec)
        {
            auto msg = fmt::format(
                FMT_STRING("Could not determine remote endpoint: {}"),
                ec.message());
            RateLimitedLog rateLimitedWarning{"TCPPeer::accept", msg};
        }
        else
        {
            result = ep.address().to_string();
        }
        return result;
    };

    auto ip = extractIP(socket);
    if (ip.empty())
    {
        return nullptr;
    }

    // First check if there's enough space to accept peer
    // If not, do not even create a peer instance as to not trigger any
    // additional reads and memory allocations
    if (!app.getOverlayManager().haveSpaceForConnection(ip))
    {
        return nullptr;
    }

    shared_ptr<TCPPeer> result;
    asio::error_code ec;
    asio::error_code lingerEc;

    asio::ip::tcp::no_delay nodelay(true);
    asio::ip::tcp::socket::linger linger(false, 0);
    std::ignore = socket->next_layer().set_option(nodelay, ec);
    std::ignore = socket->next_layer().set_option(linger, lingerEc);

    if (!ec && !lingerEc)
    {
        CLOG_DEBUG(Overlay, "TCPPeer:accept");
        result = make_shared<TCPPeer>(app, REMOTE_CALLED_US, socket, ip);
        result->initialize(PeerBareAddress{ip, 0});

        // Use weak_ptr here in case the peer is dropped before main thread
        // saves strong reference in pending/authenticated peer lists
        if (result->useBackgroundThread())
        {
            auto weak = std::weak_ptr<TCPPeer>(result);
            app.postOnOverlayThread(
                [weak]() {
                    auto result = weak.lock();
                    if (result)
                    {
                        result->startRead();
                    }
                },
                "TCPPeer::accept startRead");
        }
        else
        {
            result->startRead();
        }
    }
    else
    {
        CLOG_DEBUG(Overlay, "TCPPeer:accept error {}",
                   ec ? ec.message() : lingerEc.message());
    }

    return result;
}

TCPPeer::~TCPPeer()
{
    releaseAssert(threadIsMain());
    cancelTimers();
    if (mRole == REMOTE_CALLED_US)
    {
        (*mLiveInboundPeersCounter)--;
    }

    // If we got here, either all background threads must be joined by now, or
    // main thread just released the last reference to TCPPeer. In both
    // situations, closing the socket here is safe as main thread keeps the last
    // reference to the object (even though asio::tcp::socket is not
    // thread-safe).
    if (mSocket)
    {
        // Ignore: this indicates an attempt to cancel events
        // on a not-established socket.
        asio::error_code ec;

#ifndef _WIN32
        // This always fails on windows and ASIO won't
        // even build it.
        std::ignore = mSocket->next_layer().cancel(ec);
#endif
        std::ignore = mSocket->close(ec);
    }
}

void
TCPPeer::sendMessage(xdr::msg_ptr&& xdrBytes)
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());

    TimestampedMessage msg;
    msg.mEnqueuedTime = mAppConnector.now();
    msg.mMessage = std::move(xdrBytes);
    mThreadVars.getWriteQueue().emplace_back(std::move(msg));

    if (!mThreadVars.isWriting())
    {
        mThreadVars.setWriting(true);
        messageSender();
    }
}

void
TCPPeer::shutdown()
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());

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
    std::ignore = mSocket->next_layer().shutdown(
        asio::ip::tcp::socket::shutdown_both, ec);
    if (ec)
    {
        CLOG_DEBUG(Overlay, "TCPPeer::drop shutdown socket failed: {}",
                   ec.message());
    }

    auto socketClose = [](std::shared_ptr<Peer> self) {
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
        auto self_ = std::static_pointer_cast<TCPPeer>(self);
        asio::error_code ec2;
        self_->mSocket->close(ec2);
        if (ec2)
        {
            CLOG_DEBUG(Overlay, "TCPPeer::drop close socket failed: {}",
                       ec2.message());
        }
    };

    maybeExecuteInBackground("TCPPeer: close", socketClose);
}

void
TCPPeer::messageSender()
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !useBackgroundThread());

    // if nothing to do, mark progress and return.
    if (mThreadVars.getWriteQueue().empty())
    {
        mThreadVars.setWriting(false);
        return;
    }

    // Take a snapshot of the contents of mWriteQueue into mWriteBuffers, in
    // terms of asio::const_buffers pointing into the elements of mWriteQueue,
    // and then issue a single multi-buffer ("scatter-gather") async_write that
    // covers the whole snapshot. We'll get called back when the batch is
    // completed, at which point we'll clear mWriteBuffers and remove the entire
    // snapshot worth of corresponding messages from mWriteQueue (though it may
    // have grown a bit in the meantime -- we remove only a prefix).
    releaseAssert(mThreadVars.getWriteBuffers().empty());
    auto now = mAppConnector.now();
    size_t expected_length = 0;
    size_t maxQueueSize = mAppConnector.getConfig().MAX_BATCH_WRITE_COUNT;
    releaseAssert(maxQueueSize > 0);
    size_t const maxTotalBytes =
        mAppConnector.getConfig().MAX_BATCH_WRITE_BYTES;
    for (auto& tsm : mThreadVars.getWriteQueue())
    {
        tsm.mIssuedTime = now;
        size_t sz = tsm.mMessage->raw_size();
        mThreadVars.getWriteBuffers().emplace_back(tsm.mMessage->raw_data(),
                                                   sz);
        expected_length += sz;
        mEnqueueTimeOfLastWrite = tsm.mEnqueuedTime;
        // check if we reached any limit
        if (expected_length >= maxTotalBytes)
            break;
        if (--maxQueueSize == 0)
            break;
    }

    CLOG_DEBUG(Overlay, "messageSender {} - b:{} n:{}/{}", mIPAddress,
               expected_length, mThreadVars.getWriteBuffers().size(),
               mThreadVars.getWriteQueue().size());
    mOverlayMetrics.mAsyncWrite.Mark();
    mPeerMetrics.mAsyncWrite++;
    auto self = static_pointer_cast<TCPPeer>(shared_from_this());
    asio::async_write(
        *(mSocket.get()), mThreadVars.getWriteBuffers(),
        [self, expected_length](asio::error_code const& ec,
                                std::size_t length) {
            releaseAssert(!threadIsMain() || !self->useBackgroundThread());
            if (expected_length != length)
            {
                self->drop("error during async_write",
                           Peer::DropDirection::WE_DROPPED_REMOTE);
                return;
            }
            self->writeHandler(ec, length,
                               self->mThreadVars.getWriteBuffers().size());

            // Walk through a _prefix_ of the write queue
            // _corresponding_ to the write buffers we just sent.
            // While walking, record the sent-time in metrics, but
            // also advance iterator 'i' so we wind up with an
            // iterator range to erase from the front of the write
            // queue.
            auto now = self->mAppConnector.now();
            auto i = self->mThreadVars.getWriteQueue().begin();
            while (!self->mThreadVars.getWriteBuffers().empty())
            {
                i->mCompletedTime = now;
                i->recordWriteTiming(self->mOverlayMetrics, self->mPeerMetrics);
                ++i;
                self->mThreadVars.getWriteBuffers().pop_back();
            }

            // Erase the messages from the write queue that we
            // just forgot about the buffers for.
            self->mThreadVars.getWriteQueue().erase(
                self->mThreadVars.getWriteQueue().begin(), i);

            // continue processing the queue
            if (!ec)
            {
                self->messageSender();
            }
        });
}

void
TCPPeer::TimestampedMessage::recordWriteTiming(OverlayMetrics& metrics,
                                               PeerMetrics& peerMetrics)
{
    auto qdelay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        mIssuedTime - mEnqueuedTime);
    auto wdelay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        mCompletedTime - mIssuedTime);
    metrics.mMessageDelayInWriteQueueTimer.Update(qdelay);
    metrics.mMessageDelayInAsyncWriteTimer.Update(wdelay);
    peerMetrics.mMessageDelayInWriteQueueTimer.Update(qdelay);
    peerMetrics.mMessageDelayInAsyncWriteTimer.Update(wdelay);
}

void
TCPPeer::writeHandler(asio::error_code const& error,
                      std::size_t bytes_transferred,
                      size_t messages_transferred)
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !useBackgroundThread());
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);

    mLastWrite = mAppConnector.now();

    if (error)
    {
        if (isConnected(guard))
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            mOverlayMetrics.mErrorWrite.Mark();
            CLOG_ERROR(Overlay, "Error during sending message to {}",
                       mIPAddress);
        }
        drop("error during write", Peer::DropDirection::WE_DROPPED_REMOTE);
    }
    else if (bytes_transferred != 0)
    {
        mOverlayMetrics.mMessageWrite.Mark(messages_transferred);
        mOverlayMetrics.mByteWrite.Mark(bytes_transferred);

        mPeerMetrics.mMessageWrite += messages_transferred;
        mPeerMetrics.mByteWrite += bytes_transferred;
    }
}

void
TCPPeer::noteErrorReadHeader(size_t nbytes, asio::error_code const& ec)
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());
    receivedBytes(nbytes, false);
    mOverlayMetrics.mErrorRead.Mark();
    std::string msg("error reading message header: ");
    msg.append(ec.message());
    drop(msg, Peer::DropDirection::WE_DROPPED_REMOTE);
}

void
TCPPeer::noteShortReadHeader(size_t nbytes)
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());
    receivedBytes(nbytes, false);
    mOverlayMetrics.mErrorRead.Mark();
    drop("short read of message header",
         Peer::DropDirection::WE_DROPPED_REMOTE);
}

void
TCPPeer::noteFullyReadHeader()
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());
    receivedBytes(HDRSZ, false);
}

void
TCPPeer::noteErrorReadBody(size_t nbytes, asio::error_code const& ec)
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());

    receivedBytes(nbytes, false);
    mOverlayMetrics.mErrorRead.Mark();
    std::string msg("error reading message body: ");
    msg.append(ec.message());
    drop(msg, Peer::DropDirection::WE_DROPPED_REMOTE);
}

void
TCPPeer::noteShortReadBody(size_t nbytes)
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());

    receivedBytes(nbytes, false);
    mOverlayMetrics.mErrorRead.Mark();
    drop("short read of message body", Peer::DropDirection::WE_DROPPED_REMOTE);
}

void
TCPPeer::noteFullyReadBody(size_t nbytes)
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());

    receivedBytes(nbytes, true);
}

void
TCPPeer::scheduleRead()
{
    // Post to the peer-specific Scheduler a call to ::startRead below;
    // this will be throttled to try to balance input rates across peers.
    ZoneScoped;
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);

    if (mFlowControl->isThrottled())
    {
        return;
    }

    releaseAssert(canRead());

    if (shouldAbort(guard))
    {
        return;
    }

    auto cb = [self = static_pointer_cast<TCPPeer>(shared_from_this())]() {
        self->startRead();
    };

    std::string taskName =
        fmt::format(FMT_STRING("TCPPeer::startRead for {}"), mIPAddress);

    if (useBackgroundThread())
    {
        mAppConnector.postOnOverlayThread(cb, taskName);
    }
    else
    {
        mAppConnector.postOnMainThread(cb, std::move(taskName));
    }
}

void
TCPPeer::startRead()
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !useBackgroundThread());
    releaseAssert(canRead());
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);
    if (shouldAbort(guard))
    {
        return;
    }

    mThreadVars.getIncomingHeader().clear();

    CLOG_DEBUG(Overlay, "TCPPeer::startRead {} from {}", mSocket->in_avail(),
               mIPAddress);

    mThreadVars.getIncomingHeader().resize(HDRSZ);

    // We read large-ish (256KB) buffers of data from TCP which might have quite
    // a few messages in them. We want to digest as many of these
    // _synchronously_ as we can before we issue an async_read against ASIO.
    while (mSocket->in_avail() >= HDRSZ)
    {
        asio::error_code ec_hdr, ec_body;
        size_t n = mSocket->read_some(
            asio::buffer(mThreadVars.getIncomingHeader()), ec_hdr);
        if (ec_hdr)
        {
            noteErrorReadHeader(n, ec_hdr);
            return;
        }
        if (n != HDRSZ)
        {
            noteShortReadHeader(n);
            return;
        }
        size_t length = getIncomingMsgLength();

        // in_avail = amount of unread data
        if (mSocket->in_avail() >= length)
        {
            // We can finish reading a full message here synchronously,
            // which means we will count the received header bytes here.
            noteFullyReadHeader();

            // Exit early if message body size is not acceptable
            // Peer will be dropped anyway
            if (length == 0)
            {
                return;
            }

            mThreadVars.getIncomingBody().resize(length);
            n = mSocket->read_some(asio::buffer(mThreadVars.getIncomingBody()),
                                   ec_body);
            if (ec_body)
            {
                noteErrorReadBody(n, ec_body);
                return;
            }
            if (n != length)
            {
                noteShortReadBody(n);
                return;
            }
            noteFullyReadBody(length);
            if (!recvMessage())
            {
                return;
            }

            if (mFlowControl->maybeThrottleRead())
            {
                // Break and wait until more capacity frees up
                // When it does, read will get rescheduled automatically
                return;
            }
            if (!useBackgroundThread() && mAppConnector.shouldYield())
            {
                break;
            }
        }
        else
        {
            // No throttling - we just read a header, so we must have capacity
            releaseAssert(canRead());

            // We read a header synchronously, but don't have enough data in the
            // buffered_stream to read the body synchronously. Pretend we just
            // finished reading the header asynchronously, and punt to
            // readHeaderHandler to let it re-read the header and issue an async
            // read for the body.
            readHeaderHandler(asio::error_code(), HDRSZ);
            return;
        }
    }

    if (mSocket->in_avail() < HDRSZ)
    {
        // If there wasn't enough readable in the buffered stream to even get a
        // header (message length), issue an async_read and hope that the
        // buffering pulls in much more than just the 4 bytes we ask for here.
        mOverlayMetrics.mAsyncRead.Mark();
        mPeerMetrics.mAsyncRead++;
        auto self = static_pointer_cast<TCPPeer>(shared_from_this());
        asio::async_read(*(mSocket.get()),
                         asio::buffer(mThreadVars.getIncomingHeader()),
                         [self](asio::error_code ec, std::size_t length) {
                             self->readHeaderHandler(ec, length);
                         });
    }
    else
    {
        // If we get here it's because we broke out of the input loop above
        // early (via shouldYield) which means it's time to bounce off a the
        // per-peer scheduler queue to throttle further input.
        // Note: this can only happen on the main thread, because background
        // thread is outside of main thread Scheduler's discipline
        releaseAssert(threadIsMain());
        scheduleRead();
    }
}

size_t
TCPPeer::getIncomingMsgLength()
{
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);
    auto const& header = mThreadVars.getIncomingHeader();
    releaseAssert(header.size() == HDRSZ);
    size_t length = static_cast<size_t>(header[0]);
    length &= 0x7f; // clear the XDR 'continuation' bit
    length <<= 8;
    length |= header[1];
    length <<= 8;
    length |= header[2];
    length <<= 8;
    length |= header[3];
    if (length <= 0 ||
        (!isAuthenticated(guard) && (length > MAX_UNAUTH_MESSAGE_SIZE)) ||
        length > MAX_MESSAGE_SIZE)
    {
        mOverlayMetrics.mErrorRead.Mark();
        CLOG_ERROR(Overlay, "{} TCP: message size unacceptable: {}{}",
                   mIPAddress, length,
                   (isAuthenticated(guard) ? "" : " while not authenticated"));
        drop("error during read", Peer::DropDirection::WE_DROPPED_REMOTE);
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
    releaseAssert(!threadIsMain() || !useBackgroundThread());

    if (error)
    {
        noteErrorReadHeader(bytes_transferred, error);
    }
    else if (bytes_transferred != HDRSZ)
    {
        noteShortReadHeader(bytes_transferred);
    }
    else
    {
        noteFullyReadHeader();
        size_t expected_length = getIncomingMsgLength();
        if (expected_length != 0)
        {
            mThreadVars.getIncomingBody().resize(expected_length);
            auto self = static_pointer_cast<TCPPeer>(shared_from_this());
            asio::async_read(
                *mSocket.get(), asio::buffer(mThreadVars.getIncomingBody()),
                [self, expected_length](asio::error_code ec,
                                        std::size_t length) {
                    self->readBodyHandler(ec, length, expected_length);
                });
        }
    }
}

void
TCPPeer::readBodyHandler(asio::error_code const& error,
                         std::size_t bytes_transferred,
                         std::size_t expected_length)
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());

    if (error)
    {
        noteErrorReadBody(bytes_transferred, error);
    }
    else if (bytes_transferred != expected_length)
    {
        noteShortReadBody(bytes_transferred);
    }
    else
    {
        noteFullyReadBody(bytes_transferred);
        if (!recvMessage())
        {
            return;
        }
        mThreadVars.getIncomingHeader().clear();
        // Completing a startRead => readHeaderHandler => readBodyHandler
        // sequence happens after the first read of a single large input-buffer
        // worth of input. Even when we weren't preempted, we still bounce off
        // the per-peer scheduler queue here, to balance input across peers.
        if (mFlowControl->maybeThrottleRead())
        {
            // No more capacity after processing this message
            return;
        }

        scheduleRead();
    }
}

bool
TCPPeer::recvMessage()
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !useBackgroundThread());
    releaseAssert(canRead());
    std::string errorMsg;
    bool valid = false;

    try
    {
        xdr::xdr_get g(mThreadVars.getIncomingBody().data(),
                       mThreadVars.getIncomingBody().data() +
                           mThreadVars.getIncomingBody().size());
        AuthenticatedMessage am;
        xdr::xdr_argpack_archive(g, am);

        valid = Peer::recvAuthenticatedMessage(std::move(am));
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG_ERROR(Overlay, "{} - recvMessage got a corrupt xdr: {}",
                   mIPAddress, e.what());
        errorMsg = "received corrupt XDR";
    }
    catch (CryptoError const& e)
    {
        CLOG_ERROR(Overlay, "{} - Crypto error: {}", mIPAddress, e.what());
        errorMsg = "crypto error";
    }

    if (!errorMsg.empty())
    {
        auto drop = [errorMsg, self = shared_from_this()]() {
            // Queue up a drop; we may still process new messages
            // from this peer, which is harmless. Any new message processing
            // will stop once the main thread officially drops this peer.
            self->sendErrorAndDrop(ERR_DATA, errorMsg);
        };
        if (!threadIsMain())
        {
            mAppConnector.postOnMainThread(drop, "TCPPeer::recvMessage drop");
        }
        else
        {
            drop();
        }
        return false;
    }
    return valid;
}

// `drop` can be initiated from any thread and is thread-safe. The method simply
// schedules a callback on the main thread, that is going to clean up connection
// lists and remove the peer.
void
TCPPeer::drop(std::string const& reason, DropDirection dropDirection)
{
    // Attempts to set mDropStarted to true, returns previous value. If previous
    // value was false, this means we're initiating the drop for the first time
    if (mDropStarted.exchange(true))
    {
        // This isn't the first time `drop` is initiated, exit early, as we
        // already have shutdown queued up
        return;
    }

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());
    auto mainThreadDrop = [self, reason, dropDirection]() {
        self->shutdownAndRemovePeer(reason, dropDirection);
        // Close the socket with a delay
        self->getRecurrentTimer().cancel();
        self->getRecurrentTimer().expires_from_now(std::chrono::seconds(5));
        self->getRecurrentTimer().async_wait(
            [self](asio::error_code const& ec) {
                self->maybeExecuteInBackground(
                    "TCPPeer::shutdown", [](std::shared_ptr<Peer> peer) {
                        auto self = std::static_pointer_cast<TCPPeer>(peer);
                        self->shutdown();
                    });
            });
    };

    if (threadIsMain())
    {
        mainThreadDrop();
    }
    else
    {
        mAppConnector.postOnMainThread(mainThreadDrop, "TCPPeer::drop");
    }
}
}
