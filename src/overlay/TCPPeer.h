#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Peer.h"
#include "util/Timer.h"
#include <deque>

namespace medida
{
class Meter;
}

namespace stellar
{

static auto const MAX_UNAUTH_MESSAGE_SIZE = 0x1000;

// Peer that communicates via a TCP socket.
class TCPPeer : public Peer
{
  public:
    typedef asio::buffered_read_stream<asio::ip::tcp::socket> SocketType;
    static constexpr size_t BUFSZ = 0x40000; // 256KB

  private:
    // Helper class which provides invariance for various data structures;
    // Each method should be called from the same thread
    class ThreadRestrictedVars
    {
        std::deque<TimestampedMessage> mWriteQueue;
        std::vector<asio::const_buffer> mWriteBuffers;
        bool const mUseBackgroundThread;
        bool mWriting{false};
        std::vector<uint8_t> mIncomingHeader;
        std::vector<uint8_t> mIncomingBody;

      public:
        ThreadRestrictedVars(bool useBackgroundThread)
            : mUseBackgroundThread(useBackgroundThread)
        {
        }
        std::deque<TimestampedMessage>&
        getWriteQueue()
        {
            releaseAssert(!threadIsMain() || !mUseBackgroundThread);
            return mWriteQueue;
        }
        std::vector<asio::const_buffer>&
        getWriteBuffers()
        {
            releaseAssert(!threadIsMain() || !mUseBackgroundThread);
            return mWriteBuffers;
        }
        std::vector<uint8_t>&
        getIncomingHeader()
        {
            releaseAssert(!threadIsMain() || !mUseBackgroundThread);
            return mIncomingHeader;
        }
        std::vector<uint8_t>&
        getIncomingBody()
        {
            releaseAssert(!threadIsMain() || !mUseBackgroundThread);
            return mIncomingBody;
        }
        bool
        isWriting() const
        {
            releaseAssert(!threadIsMain() || !mUseBackgroundThread);
            return mWriting;
        }
        void
        setWriting(bool value)
        {
            releaseAssert(!threadIsMain() || !mUseBackgroundThread);
            mWriting = value;
        }
    };

    ThreadRestrictedVars mThreadVars;

    // Drop can be initiated from any thread only once, keep track of that with
    // an atomic
    std::atomic<bool> mDropStarted{false};
    std::shared_ptr<SocketType> mSocket;
    std::string const mIPAddress;

    bool recvMessage();
    void sendMessage(xdr::msg_ptr&& xdrBytes) override;

    void messageSender();

    size_t getIncomingMsgLength();
    virtual void connected() override;
    void scheduleRead() override;
    void startRead();

    static constexpr size_t HDRSZ = 4;
    void noteErrorReadHeader(size_t nbytes, asio::error_code const& ec);
    void noteShortReadHeader(size_t nbytes);
    void noteFullyReadHeader();
    void noteErrorReadBody(size_t nbytes, asio::error_code const& ec);
    void noteShortReadBody(size_t nbytes);
    void noteFullyReadBody(size_t nbytes);

    void writeHandler(asio::error_code const& error,
                      std::size_t bytes_transferred,
                      std::size_t messages_transferred);
    void readHeaderHandler(asio::error_code const& error,
                           std::size_t bytes_transferred);
    void readBodyHandler(asio::error_code const& error,
                         std::size_t bytes_transferred,
                         std::size_t expected_length);
    void shutdown();

    // This tracks the count of TCPPeers that are live and and originate in
    // inbound connections.
    //
    // This is subtle: TCPPeers can be kept alive by shared references stored
    // in ASIO completion events, because TCPPeers own buffers that ASIO
    // operations write into. If we stored weak references in ASIO completion
    // events, it would be possible for a TCPPeer to be destructed and
    // write-buffers freed during the ASIO write into those buffers, which
    // would cause memory corruption.
    //
    // As a retult, the lifetime of a TCPPeer is _not_ the same as the time it
    // is known to the OverlayManager. We can drop a TCPPeer from the
    // OverlayManager's registration a while before it's actually destroyed.
    // To properly manage load, therefore, we have to separately track the
    // number of actually-live TCPPeers. Since we're really only concerned
    // with load-shedding inbound connections (we make our own outbound ones),
    // we only track the inbound-live number.
    //
    // Furthermore the counter _itself_ has to be accessed as a shared pointer
    // because any other central place we might track the live count (overlay
    // manager or metrics) may be dead before the TCPPeer destructor runs.
    std::shared_ptr<int> mLiveInboundPeersCounter;

  public:
    typedef std::shared_ptr<TCPPeer> pointer;

    TCPPeer(Application& app, Peer::PeerRole role,
            std::shared_ptr<SocketType> socket,
            std::string address); // hollow
                                  // constructor; use
                                  // `initiate` or
                                  // `accept` instead

    static pointer initiate(Application& app, PeerBareAddress const& address);
    static pointer accept(Application& app, std::shared_ptr<SocketType> socket);

    virtual ~TCPPeer();

    virtual void drop(std::string const& reason,
                      DropDirection dropDirection) override;
};
}
