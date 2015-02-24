#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "util/asio.h"
#include <memory>

/*
listens for peer connections.
When found passes them to the PeerMaster
*/

namespace stellar
{
using namespace std;
class Application;
class PeerDoorStub;

class PeerDoor
{
protected:
    Application& mApp;
    asio::ip::tcp::acceptor mAcceptor;

    virtual void acceptNextPeer();
    virtual void handleKnock(std::shared_ptr<asio::ip::tcp::socket> pSocket);

    friend PeerDoorStub;
  public:
    typedef shared_ptr<PeerDoor> pointer;
      
    PeerDoor(Application&);

    void close();
};
}


