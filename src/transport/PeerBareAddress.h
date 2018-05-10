#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "xdr/Stellar-overlay.h"

namespace stellar
{

class Application;

class PeerBareAddress
{
  public:
    enum class Type
    {
        EMPTY,
        IPv4
    };

    PeerBareAddress();
    explicit PeerBareAddress(std::string ip, unsigned short port);
    explicit PeerBareAddress(PeerAddress const& pa);

    static PeerBareAddress
    resolve(std::string const& ipPort, Application& app,
            unsigned short defaultPort = DEFAULT_PEER_PORT);

    bool
    isEmpty() const
    {
        return mType == Type::EMPTY;
    }

    Type
    getType() const
    {
        return mType;
    }

    std::string const&
    getIP() const
    {
        return mIP;
    }

    unsigned short
    getPort() const
    {
        return mPort;
    }

    std::string toString() const;
    void toXdr(PeerAddress& ret) const;

    bool isPrivate() const;
    bool isLocalhost() const;

    friend bool operator==(PeerBareAddress const& x, PeerBareAddress const& y);
    friend bool operator!=(PeerBareAddress const& x, PeerBareAddress const& y);

  private:
    Type mType;
    std::string mIP;
    unsigned short mPort;
};
}
