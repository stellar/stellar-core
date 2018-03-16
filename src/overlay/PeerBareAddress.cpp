// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "overlay/PeerBareAddress.h"
#include "main/Application.h"
#include "util/Logging.h"

#include <lib/util/format.h>
#include <regex>
#include <sstream>

namespace stellar
{

namespace
{

void
ipToXdr(std::string const& ip, xdr::opaque_array<4U>& ret)
{
    std::stringstream ss(ip);
    std::string item;
    int n = 0;
    while (getline(ss, item, '.') && n < 4)
    {
        ret[n] = static_cast<unsigned char>(atoi(item.c_str()));
        n++;
    }
    if (n != 4)
        throw std::runtime_error("ipToXdr: failed on `" + ip + "`");
}
}

PeerBareAddress::PeerBareAddress() : mType{Type::EMPTY}
{
}

PeerBareAddress::PeerBareAddress(std::string ip, unsigned short port)
    : mType{Type::IPv4}, mIP{std::move(ip)}, mPort{port}
{
    if (mIP.empty())
    {
        throw std::runtime_error("Cannot create PeerBareAddress with empty ip");
    }
    if (mPort == 0)
    {
        throw std::runtime_error("Cannot create PeerBareAddress with port 0");
    }
}

PeerBareAddress::PeerBareAddress(PeerAddress const& pa) : mType{Type::IPv4}
{
    assert(pa.ip.type() == IPv4);

    std::stringstream ip;
    ip << (int)pa.ip.ipv4()[0] << "." << (int)pa.ip.ipv4()[1] << "."
       << (int)pa.ip.ipv4()[2] << "." << (int)pa.ip.ipv4()[3];
    mIP = ip.str();
    mPort = static_cast<unsigned short>(pa.port);
}

PeerBareAddress
PeerBareAddress::resolve(std::string const& ipPort, Application& app,
                         unsigned short defaultPort)
{
    static std::regex re(
        "^(?:(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3})|([[:alnum:].-]+))"
        "(?:\\:(\\d{1,5}))?$");
    std::smatch m;

    if (!std::regex_search(ipPort, m, re) || m.empty())
    {
        throw std::runtime_error(
            fmt::format("Cannot parse peer address '{}'", ipPort));
    }

    asio::ip::tcp::resolver::query::flags resolveflags;
    std::string toResolve;
    if (m[1].matched)
    {
        resolveflags = asio::ip::tcp::resolver::query::flags::numeric_host;
        toResolve = m[1].str();
    }
    else
    {
        resolveflags = asio::ip::tcp::resolver::query::flags::v4_mapped;
        toResolve = m[2].str();
    }

    asio::ip::tcp::resolver resolver(app.getWorkerIOService());
    asio::ip::tcp::resolver::query query(toResolve, "", resolveflags);

    asio::error_code ec;
    asio::ip::tcp::resolver::iterator i = resolver.resolve(query, ec);
    if (ec)
    {
        LOG(DEBUG) << "Could not resolve '" << ipPort << "' : " << ec.message();
        throw std::runtime_error(
            fmt::format("Could not resolve '{}': {}", ipPort, ec.message()));
    }

    std::string ip;
    while (i != asio::ip::tcp::resolver::iterator())
    {
        asio::ip::tcp::endpoint end = *i;
        if (end.address().is_v4())
        {
            ip = end.address().to_v4().to_string();
            break;
        }
        i++;
    }
    if (ip.empty())
    {
        throw std::runtime_error(
            fmt::format("Could not resolve '{}': {}", ipPort, ec.message()));
    }

    unsigned short port = defaultPort;
    if (m[3].matched)
    {
        int parsedPort = atoi(m[3].str().c_str());
        if (parsedPort <= 0 || parsedPort > UINT16_MAX)
        {
            throw std::runtime_error(fmt::format("Could not resolve '{}': {}",
                                                 ipPort, ec.message()));
        }
        port = static_cast<unsigned short>(parsedPort);
    }

    assert(!ip.empty());
    assert(port != 0);

    return PeerBareAddress{ip, port};
}

std::string
PeerBareAddress::toString() const
{
    switch (mType)
    {
    case Type::EMPTY:
    {
        return "(empty)";
    }
    case Type::IPv4:
    {
        return mIP + ":" + std::to_string(mPort);
    }
    default:
        abort();
    }
}

bool
PeerBareAddress::isPrivate() const
{
    asio::error_code ec;
    asio::ip::address_v4 addr = asio::ip::address_v4::from_string(mIP, ec);
    if (ec)
    {
        return false;
    }
    unsigned long val = addr.to_ulong();
    if (((val >> 24) == 10)        // 10.x.y.z
        || ((val >> 20) == 2753)   // 172.[16-31].x.y
        || ((val >> 16) == 49320)) // 192.168.x.y
    {
        return true;
    }
    return false;
}

bool
PeerBareAddress::isLocalhost() const
{
    return mIP == "127.0.0.1";
}

void
PeerBareAddress::toXdr(PeerAddress& ret) const
{
    ret.port = mPort;
    ret.ip.type(IPv4);
    ipToXdr(mIP, ret.ip.ipv4());
}

bool
operator==(PeerBareAddress const& x, PeerBareAddress const& y)
{
    if (x.mIP != y.mIP)
    {
        return false;
    }
    if (x.mPort != y.mPort)
    {
        return false;
    }

    return true;
}

bool
operator!=(PeerBareAddress const& x, PeerBareAddress const& y)
{
    return !(x == y);
}
}
