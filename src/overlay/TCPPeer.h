#ifndef __TCPPEER__
#define __TCPPEER__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "overlay/Peer.h"
#include "util/Timer.h"

namespace stellar
{
// Peer that communicates via a TCP socket.
class TCPPeer : public Peer
{
    std::shared_ptr<asio::ip::tcp::socket> mSocket;
    VirtualTimer mHelloTimer;
    uint8_t mIncomingHeader[4];
    std::vector<uint8_t> mIncomingBody;

    void recvMessage();
    void recvHello(StellarMessage const& msg);
    void sendMessage(xdr::msg_ptr&& xdrBytes);
    int getIncomingMsgLength();
    void startRead();

    void writeHandler(const asio::error_code& error,
                      std::size_t bytes_transferred);
    void readHeaderHandler(const asio::error_code& error,
                           std::size_t bytes_transferred);
    void readBodyHandler(const asio::error_code& error,
                         std::size_t bytes_transferred);

    static const char* kSQLCreateStatement;

  public:
    TCPPeer(Application& app, std::shared_ptr<asio::ip::tcp::socket> socket);
    TCPPeer(Application& app, std::string& ip, int port);

    virtual ~TCPPeer()
    {
    }

    void drop();
    std::string getIP();
};
}

#endif
