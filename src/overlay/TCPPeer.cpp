// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TCPPeer.h"
#include "crypto/CryptoError.h"
#include "crypto/Curve25519.h"
#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/PeerManager.h"
#include "overlay/StellarXDR.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>
#include <fmt/format.h>

using namespace soci;

namespace stellar
{

using namespace std;

///////////////////////////////////////////////////////////////////////
// TCPPeer - main thread wrapper around asio TCP socket (socket operations have
// an option to run in the background)
///////////////////////////////////////////////////////////////////////

TCPPeer::TCPPeer(Application& app, Peer::PeerRole role,
                 std::shared_ptr<TCPPeer::SocketType> socket)
    : Peer(app, role)
    , mSocket(socket)
    , mBackgroundWriteQueue(useBackgroundThread())
    , mLiveInboundPeersCounter(
          app.getOverlayManager().getLiveInboundPeersCounter())
    , mShutdownTimer(app.getOverlayIOContext())
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
    auto socket = make_shared<SocketType>(app.getOverlayIOContext(), BUFSZ);
    auto result = make_shared<TCPPeer>(app, WE_CALLED_REMOTE, socket);
    result->mAddress = address;
    result->startRecurrentTimer();
    asio::ip::tcp::endpoint endpoint(
        asio::ip::address::from_string(address.getIP()), address.getPort());
    socket->next_layer().async_connect(
        endpoint, [result](asio::error_code const& error) {
            releaseAssert(!threadIsMain() || !result->useBackgroundThread());

            // We might have been dropped while waiting to connect; in this
            // case, do not execute handler and just exit
            if (result->shouldAbort())
            {
                return;
            }
            asio::error_code ec;
            asio::error_code lingerEc;
            if (!error)
            {
                asio::ip::tcp::no_delay nodelay(true);
                asio::ip::tcp::socket::linger linger(false, 0);
                result->mSocket->next_layer().set_option(nodelay, ec);
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

    // First check if there's enough space to accept peer
    // If not, do not even create a peer instance as to not trigger any
    // additional reads and memory allocations
    if (!app.getOverlayManager().haveSpaceForConnection(TCPPeer::getIP(socket)))
    {
        return nullptr;
    }

    shared_ptr<TCPPeer> result;
    asio::error_code ec;
    asio::error_code lingerEc;

    asio::ip::tcp::no_delay nodelay(true);
    asio::ip::tcp::socket::linger linger(false, 0);
    socket->next_layer().set_option(nodelay, ec);
    socket->next_layer().set_option(linger, lingerEc);

    if (!ec && !lingerEc)
    {
        CLOG_DEBUG(Overlay, "TCPPeer:accept");
        result = make_shared<TCPPeer>(app, REMOTE_CALLED_US, socket);
        result->mAddress = PeerBareAddress{result->getIP(), 0};
        result->startRecurrentTimer();
        auto f = [weak = std::weak_ptr<TCPPeer>(result)]() {
            auto self = weak.lock();
            if (self)
            {
                self->startRead();
            }
        };
        // this will be destroyed from background
        app.postOnOverlayThread(f, "startRead");
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
    if (!threadIsMain())
    {
        CLOG_INFO(Tx, "NOT MAIN ");
    }
    releaseAssert(threadIsMain());
    Peer::shutdown();
    if (mRole == REMOTE_CALLED_US)
    {
        (*mLiveInboundPeersCounter)--;
    }

    // But the time we're here, it should be main thread,
    // AND background thread should be already stopped
    // so not concurrent socket manipulations are happening

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

std::string
TCPPeer::getIP() const
{
    return getIP(mSocket);
}

std::string
TCPPeer::getIP(std::shared_ptr<SocketType> socket)
{
    std::string result;

    asio::error_code ec;
    auto ep = socket->next_layer().remote_endpoint(ec);
    if (ec)
    {
        CLOG_ERROR(Overlay, "Could not determine remote endpoint: {}",
                   ec.message());
    }
    else
    {
        result = ep.address().to_string();
    }

    return result;
}

void
TCPPeer::sendMessage(xdr::msg_ptr&& xdrBytes)
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());

    if (shouldAbort())
    {
        return;
    }

    TimestampedMessage msg;
    msg.mEnqueuedTime = mApp.getClock().now();
    msg.mMessage = std::move(xdrBytes);
    mBackgroundWriteQueue.getWriteQueue().emplace_back(std::move(msg));

    if (!mWriting)
    {
        mWriting = true;
        messageSender();
    }
}

void
TCPPeer::shutdown()
{
    if (mShutdownScheduled)
    {
        // should not happen, leave here for debugging purposes
        CLOG_ERROR(Overlay, "Double schedule of shutdown {}", toString());
        CLOG_ERROR(Overlay, "{}", REPORT_INTERNAL_BUG);
        return;
    }

    mShutdownScheduled = true;
    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    // To shutdown, we first queue up our desire to shutdown in the strand,
    // behind any pending read/write calls. We'll let them issue first.

    // leave some time before actually calling shutdown
    mShutdownTimer.expires_from_now(std::chrono::seconds(5));
    mShutdownTimer.async_wait([self](asio::error_code) {
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
            CLOG_DEBUG(Overlay, "TCPPeer::drop shutdown socket failed: {}",
                       ec.message());
        }
        self->getApp().postOnOverlayThread(
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
                    CLOG_DEBUG(Overlay, "TCPPeer::drop close socket failed: {}",
                               ec2.message());
                }
            },
            "TCPPeer: close");
    });
}

void
TCPPeer::messageSender()
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !useBackgroundThread());

    // if nothing to do, mark progress and return.
    if (mBackgroundWriteQueue.getWriteQueue().empty())
    {
        mWriting = false;
        // there is nothing to send and delayed shutdown was
        // requested - time to perform it
        if (mDelayedShutdown)
        {
            shutdown();
        }
        return;
    }

    // Take a snapshot of the contents of mBackgroundWriteQueue.getWriteQueue()
    // into mBackgroundWriteQueue.getWriteBuffers(), in terms of
    // asio::const_buffers pointing into the elements of
    // mBackgroundWriteQueue.getWriteQueue(), and then issue a single
    // multi-buffer ("scatter-gather") async_write that covers the whole
    // snapshot. We'll get called back when the batch is completed, at which
    // point we'll clear mBackgroundWriteQueue.getWriteBuffers() and remove the
    // entire snapshot worth of corresponding messages from
    // mBackgroundWriteQueue.getWriteQueue() (though it may have grown a bit in
    // the meantime -- we remove only a prefix).
    releaseAssert(mBackgroundWriteQueue.getWriteBuffers().empty());
    auto now = mApp.getClock().now();
    size_t expected_length = 0;
    size_t maxQueueSize = mConfig.MAX_BATCH_WRITE_COUNT;
    releaseAssert(maxQueueSize > 0);
    size_t const maxTotalBytes = mConfig.MAX_BATCH_WRITE_BYTES;
    for (auto& tsm : mBackgroundWriteQueue.getWriteQueue())
    {
        tsm.mIssuedTime = now;
        size_t sz = tsm.mMessage->raw_size();
        mBackgroundWriteQueue.getWriteBuffers().emplace_back(
            tsm.mMessage->raw_data(), sz);
        expected_length += sz;
        mEnqueueTimeOfLastWrite = tsm.mEnqueuedTime;
        // check if we reached any limit
        if (expected_length >= maxTotalBytes)
            break;
        if (--maxQueueSize == 0)
            break;
    }

    CLOG_DEBUG(Overlay, "messageSender {} - b:{} n:{}/{}", toString(),
               expected_length, mBackgroundWriteQueue.getWriteBuffers().size(),
               mBackgroundWriteQueue.getWriteQueue().size());
    getOverlayMetrics().mAsyncWrite.Mark();
    mPeerMetrics.mAsyncWrite++;
    std::weak_ptr<TCPPeer> weak(
        std::static_pointer_cast<TCPPeer>(shared_from_this()));
    asio::async_write(
        *(mSocket.get()), mBackgroundWriteQueue.getWriteBuffers(),
        [weak, expected_length](asio::error_code const& ec,
                                std::size_t length) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }

            releaseAssert(!threadIsMain() || !self->useBackgroundThread());
            if (expected_length != length)
            {
                self->drop("error during async_write",
                           Peer::DropDirection::WE_DROPPED_REMOTE,
                           Peer::DropMode::IGNORE_WRITE_QUEUE);
                return;
            }
            self->writeHandler(
                ec, length,
                self->mBackgroundWriteQueue.getWriteBuffers().size());

            // Walk through a _prefix_ of the write queue
            // _corresponding_ to the write buffers we just sent.
            // While walking, record the sent-time in metrics, but
            // also advance iterator 'i' so we wind up with an
            // iterator range to erase from the front of the write
            // queue.
            auto now = self->mApp.getClock().now();
            auto i = self->mBackgroundWriteQueue.getWriteQueue().begin();
            while (!self->mBackgroundWriteQueue.getWriteBuffers().empty())
            {
                i->mCompletedTime = now;
                i->recordWriteTiming(self->getOverlayMetrics(),
                                     self->mPeerMetrics);
                ++i;
                self->mBackgroundWriteQueue.getWriteBuffers().pop_back();
            }

            // Erase the messages from the write queue that we
            // just forgot about the buffers for.
            self->mBackgroundWriteQueue.getWriteQueue().erase(
                self->mBackgroundWriteQueue.getWriteQueue().begin(), i);

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
    mLastWrite = mApp.getClock().now();

    if (error)
    {
        if (isConnected())
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            getOverlayMetrics().mErrorWrite.Mark();
            CLOG_ERROR(Overlay, "Error during sending message to {}",
                       toString());
        }
        if (mDelayedShutdown)
        {
            // delayed shutdown was requested - time to perform it
            shutdown();
        }
        else
        {
            // no delayed shutdown - we can drop normally
            drop("error during write", Peer::DropDirection::WE_DROPPED_REMOTE,
                 Peer::DropMode::IGNORE_WRITE_QUEUE);
        }
    }
    else if (bytes_transferred != 0)
    {
        getOverlayMetrics().mMessageWrite.Mark(messages_transferred);
        getOverlayMetrics().mByteWrite.Mark(bytes_transferred);
        mPeerMetrics.mMessageWrite += messages_transferred;
        mPeerMetrics.mByteWrite += bytes_transferred;
    }
}

void
TCPPeer::noteErrorReadHeader(size_t nbytes, asio::error_code const& ec)
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());
    receivedBytes(nbytes, false);
    getOverlayMetrics().mErrorRead.Mark();
    std::string msg("error reading message header: ");
    msg.append(ec.message());
    drop(msg, Peer::DropDirection::WE_DROPPED_REMOTE,
         Peer::DropMode::IGNORE_WRITE_QUEUE);
}

void
TCPPeer::noteShortReadHeader(size_t nbytes)
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());
    receivedBytes(nbytes, false);
    getOverlayMetrics().mErrorRead.Mark();
    drop("short read of message header", Peer::DropDirection::WE_DROPPED_REMOTE,
         Peer::DropMode::IGNORE_WRITE_QUEUE);
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
    getOverlayMetrics().mErrorRead.Mark();
    std::string msg("error reading message body: ");
    msg.append(ec.message());
    drop(msg, Peer::DropDirection::WE_DROPPED_REMOTE,
         Peer::DropMode::IGNORE_WRITE_QUEUE);
}

void
TCPPeer::noteShortReadBody(size_t nbytes)
{
    releaseAssert(!threadIsMain() || !useBackgroundThread());

    receivedBytes(nbytes, false);
    getOverlayMetrics().mErrorRead.Mark();
    drop("short read of message body", Peer::DropDirection::WE_DROPPED_REMOTE,
         Peer::DropMode::IGNORE_WRITE_QUEUE);
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
    releaseAssert(!threadIsMain() || !useBackgroundThread());

    if (mLastThrottle || !canRead() || shouldAbort())
    {
        return;
    }

    std::weak_ptr<TCPPeer> weak(
        std::static_pointer_cast<TCPPeer>(shared_from_this()));
    auto cb = [weak]() {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        self->startRead();
    };

    std::string taskName =
        fmt::format(FMT_STRING("TCPPeer::startRead for {}"), toString());

    if (useBackgroundThread())
    {
        getApp().postOnOverlayThread(cb, taskName);
    }
    else
    {
        getApp().postOnMainThread(cb, std::move(taskName));
    }
}

void
TCPPeer::startRead()
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !useBackgroundThread());
    releaseAssert(canRead());
    if (shouldAbort())
    {
        return;
    }

    mIncomingHeader.clear();

    CLOG_DEBUG(Overlay, "TCPPeer::startRead {} from {}", mSocket->in_avail(),
               toString());

    mIncomingHeader.resize(HDRSZ);

    // We read large-ish (256KB) buffers of data from TCP which might have quite
    // a few messages in them. We want to digest as many of these
    // _synchronously_ as we can before we issue an async_read against ASIO.
    while (mSocket->in_avail() >= HDRSZ)
    {
        asio::error_code ec_hdr, ec_body;
        size_t n = mSocket->read_some(asio::buffer(mIncomingHeader), ec_hdr);
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
            if (length != 0)
            {
                mIncomingBody.resize(length);
                n = mSocket->read_some(asio::buffer(mIncomingBody), ec_body);
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
                recvMessage();
                if (!canRead())
                {
                    // Break and wait until more capacity frees up
                    // When it does, read will get rescheduled automatically
                    CLOG_DEBUG(Overlay, "Throttle reading from peer {}!",
                               mConfig.toShortString(getPeerID()));
                    mLastThrottle = mApp.getClock().now();
                    return;
                }
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

    releaseAssert(mSocket->in_avail() < HDRSZ);
    // If there wasn't enough readable in the buffered stream to even get a
    // header (message length), issue an async_read and hope that the
    // buffering pulls in much more than just the 4 bytes we ask for here.
    getOverlayMetrics().mAsyncRead.Mark();
    mPeerMetrics.mAsyncRead++;
    std::weak_ptr<TCPPeer> weak(
        std::static_pointer_cast<TCPPeer>(shared_from_this()));
    asio::async_read(*(mSocket.get()), asio::buffer(mIncomingHeader),
                     [weak](asio::error_code ec, std::size_t length) {
                         auto self = weak.lock();
                         if (self)
                         {
                             self->readHeaderHandler(ec, length);
                         }
                     });
}

size_t
TCPPeer::getIncomingMsgLength()
{
    size_t length = static_cast<size_t>(mIncomingHeader[0]);
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
        getOverlayMetrics().mErrorRead.Mark();
        CLOG_ERROR(Overlay, "TCP: message size unacceptable: {}{}", length,
                   (isAuthenticated() ? "" : " while not authenticated"));
        drop("error during read", Peer::DropDirection::WE_DROPPED_REMOTE,
             Peer::DropMode::IGNORE_WRITE_QUEUE);
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
            mIncomingBody.resize(expected_length);
            std::weak_ptr<TCPPeer> weak(
                std::static_pointer_cast<TCPPeer>(shared_from_this()));
            asio::async_read(*mSocket.get(), asio::buffer(mIncomingBody),
                             [weak, expected_length](asio::error_code ec,
                                                     std::size_t length) {
                                 auto self = weak.lock();
                                 if (self)
                                 {
                                     self->readBodyHandler(ec, length,
                                                           expected_length);
                                 }
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
        recvMessage();
        mIncomingHeader.clear();
        // Completing a startRead => readHeaderHandler => readBodyHandler
        // sequence happens after the first read of a single large input-buffer
        // worth of input. Even when we weren't preempted, we still bounce off
        // the per-peer scheduler queue here, to balance input across peers.
        if (!canRead())
        {
            // No more capacity after processing this message
            CLOG_DEBUG(Overlay,
                       "TCPPeer::readBodyHandler: throttle reading from {}",
                       mConfig.toShortString(getPeerID()));
            mLastThrottle = mApp.getClock().now();
            return;
        }

        scheduleRead();
    }
}

std::deque<Peer::TimestampedMessage>&
TCPPeer::OverlayThreadWriteQueue::getWriteQueue()
{
    // Write queue can only be accessed by the overlay thread
    releaseAssert(!threadIsMain() || !mUseBackgroundThread);
    return mWriteQueue;
}

std::vector<asio::const_buffer>&
TCPPeer::OverlayThreadWriteQueue::getWriteBuffers()
{
    // Write queue can only be accessed by the overlay thread
    releaseAssert(!threadIsMain() || !mUseBackgroundThread);
    return mWriteBuffers;
}

void
TCPPeer::recvMessage()
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !useBackgroundThread());
    releaseAssert(canRead());
    std::string errorMsg;

    try
    {
        xdr::xdr_get g(mIncomingBody.data(),
                       mIncomingBody.data() + mIncomingBody.size());
        AuthenticatedMessage am;
        xdr::xdr_argpack_archive(g, am);

        Peer::recvMessage(std::move(am));
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG_ERROR(Overlay, "{} - recvMessage got a corrupt xdr: {}",
                   toString(), e.what());
        errorMsg = "received corrupt XDR";
    }
    catch (CryptoError const& e)
    {
        CLOG_ERROR(Overlay, "{} - Crypto error: {}", toString(), e.what());
        errorMsg = "crypto error";
    }

    if (!errorMsg.empty())
    {
        if (useBackgroundThread())
        {
            std::weak_ptr<TCPPeer> weak(
                std::static_pointer_cast<TCPPeer>(shared_from_this()));
            getApp().postOnMainThread(
                [weak, errorMsg]() {
                    auto self = weak.lock();
                    if (self)
                    {
                        // Queue up a drop; we may still process new messages
                        // from this peer, but it'll be dropped as soon as main
                        // thread gets to it
                        self->sendErrorAndDrop(
                            ERR_DATA, errorMsg,
                            Peer::DropMode::IGNORE_WRITE_QUEUE);
                    }
                },
                ACTION_QUEUE_NAME);
        }
        else
        {
            sendErrorAndDrop(ERR_DATA, errorMsg,
                             Peer::DropMode::IGNORE_WRITE_QUEUE);
        }
    }
}

void
TCPPeer::drop(std::string const& reason, DropDirection dropDirection,
              DropMode dropMode)
{
    std::lock_guard<std::recursive_mutex> guard(
        mOverlayManager.getOverlayManagerMutex());

    if (shouldAbort())
    {
        return;
    }

    if (mState != GOT_AUTH)
    {
        CLOG_DEBUG(Overlay,
                   "TCPPeer::drop {} in state {} we called:{}, reason: {}",
                   toString(), format_as(mState), format_as(mRole), reason);
    }
    else if (dropDirection == Peer::DropDirection::WE_DROPPED_REMOTE)
    {
        CLOG_INFO(Overlay, "Dropping peer {}, reason {}", toString(), reason);
    }
    else
    {
        CLOG_INFO(Overlay, "peer {} dropped us, reason {}", toString(), reason);
    }

    mState = CLOSING;

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    mOverlayManager.removePeer(this);

    // if write queue is not empty, messageSender will take care of shutdown
    if ((dropMode == Peer::DropMode::IGNORE_WRITE_QUEUE) || !mWriting)
    {
        self->shutdown();
    }
    else
    {
        self->mDelayedShutdown = true;
    }
}
}
