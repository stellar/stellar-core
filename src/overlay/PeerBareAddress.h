// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "main/Config.h"
#include "xdr/Stellar-overlay.h"

namespace stellar
{

class Application;

class PeerBareAddress
{
  public:
    explicit PeerBareAddress(asio::ip::address ip, unsigned short port);
    explicit PeerBareAddress(PeerAddress const& pa);

    static PeerBareAddress
    resolve(std::string const& ipPort, Application& app,
            unsigned short defaultPort = DEFAULT_PEER_PORT);

    asio::ip::address const&
    getIP() const
    {
        return mIP;
    }

    unsigned short
    getPort() const
    {
        return mPort;
    }

    std::string const& toString() const;

    bool isPrivate() const;
    bool isLocalhost() const;

    friend bool operator==(PeerBareAddress const& x, PeerBareAddress const& y);
    friend bool operator!=(PeerBareAddress const& x, PeerBareAddress const& y);
    friend bool operator<(PeerBareAddress const& x, PeerBareAddress const& y);

  private:
    asio::ip::address mIP;
    unsigned short mPort;
    std::string mStringValue;
};
}
