#ifndef __PEERDOOR__
#define __PEERDOOR__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#ifndef ASIO_SEPARATE_COMPILATION
#define ASIO_SEPARATE_COMPILATION
#endif
#include <asio.hpp>
#include <memory>

/*
listens for peer connections.
When found passes them to the PeerMaster
*/

namespace stellar
{
class Application;

class PeerDoor
{
    Application& mApp;
    asio::ip::tcp::acceptor mAcceptor;

    void acceptNextPeer();
    void handleKnock(std::shared_ptr<asio::ip::tcp::socket> pSocket);

  public:
    PeerDoor(Application&);

    void start();
    void close();
};
}

#endif
