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
static auto const MAX_MESSAGE_SIZE = 0x1000000;

// Peer that communicates via a TCP socket.
class TCPPeer : public Peer
{
  public:
    typedef asio::buffered_read_stream<asio::ip::tcp::socket> SocketType;
    static constexpr size_t BUFSZ = 0x40000; // 256KB

  private:
    std::shared_ptr<SocketType> mSocket;
    std::vector<uint8_t> mIncomingHeader;
    std::vector<uint8_t> mIncomingBody;

    std::vector<asio::const_buffer> mWriteBuffers;
    std::deque<TimestampedMessage> mWriteQueue;
    bool mWriting{false};
    bool mDelayedShutdown{false};
    bool mShutdownScheduled{false};

    void recvMessage();
    void sendMessage(xdr::msg_ptr&& xdrBytes) override;

    void messageSender();

    size_t getIncomingMsgLength();
    virtual void connected() override;
    void scheduleRead();
    virtual bool sendQueueIsOverloaded() const override;
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
                      std::size_t messages_transferred) override;
    void readHeaderHandler(asio::error_code const& error,
                           std::size_t bytes_transferred) override;
    void readBodyHandler(asio::error_code const& error,
                         std::size_t bytes_transferred,
                         std::size_t expected_length) override;
    void shutdown();

  public:
    typedef std::shared_ptr<TCPPeer> pointer;

    TCPPeer(Application& app, Peer::PeerRole role,
            std::shared_ptr<SocketType> socket); // hollow
                                                 // constuctor; use
                                                 // `initiate` or
                                                 // `accept` instead

    static pointer initiate(Application& app, PeerBareAddress const& address);
    static pointer accept(Application& app, std::shared_ptr<SocketType> socket);

    virtual ~TCPPeer();

    virtual void drop(std::string const& reason, DropDirection dropDirection,
                      DropMode dropMode) override;

    std::string getIP() const override;
};
}
