#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "overlay/Peer.h"
#include "util/Timer.h"

namespace medida { class Meter; }

namespace stellar
{
// Peer that communicates via a TCP socket.
class TCPPeer : public Peer
{
    string mIP;
    std::shared_ptr<asio::ip::tcp::socket> mSocket;
    VirtualTimer mHelloTimer;
    uint8_t mIncomingHeader[4];
    std::vector<uint8_t> mIncomingBody;

    medida::Meter& mMessageRead;
    medida::Meter& mMessageWrite;
    medida::Meter& mByteRead;
    medida::Meter& mByteWrite;

    void timerExpired(const asio::error_code & error);
    void recvMessage();
    void recvHello(StellarMessage const& msg);
    void sendMessage(xdr::msg_ptr&& xdrBytes);
    int getIncomingMsgLength();
    virtual void connected() override;
    void startRead();

    void writeHandler(const asio::error_code& error,
                      std::size_t bytes_transferred);
    void readHeaderHandler(const asio::error_code& error,
                           std::size_t bytes_transferred);
    void readBodyHandler(const asio::error_code& error,
                         std::size_t bytes_transferred);

  public:
    typedef shared_ptr<TCPPeer> pointer;

    TCPPeer(Application& app, Peer::PeerRole role,
        std::shared_ptr<asio::ip::tcp::socket> socket); // hollow constuctor; use `initiate` or `accept` instead

    static pointer initiate(Application& app, const std::string& ip, int port);
    static pointer accept(Application& app, shared_ptr<asio::ip::tcp::socket> socket);


    virtual ~TCPPeer();

    virtual void drop() override;
    virtual std::string getIP() override;
};
}
