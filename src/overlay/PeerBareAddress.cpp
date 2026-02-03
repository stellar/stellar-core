// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerBareAddress.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"

#include <fmt/format.h>
#include <regex>

namespace
{
std::string
formatAddress(asio::ip::address const& ip, unsigned short port)
{
    if (ip.is_v6())
    {
        return fmt::format(FMT_STRING("[{}]:{:d}"), ip.to_string(), port);
    }
    return fmt::format(FMT_STRING("{}:{:d}"), ip.to_string(), port);
}
} // namespace

namespace stellar
{

PeerBareAddress::PeerBareAddress(asio::ip::address ip, unsigned short port)
    : mIP{std::move(ip)}, mPort{port}, mStringValue{formatAddress(mIP, mPort)}
{
}

PeerBareAddress::PeerBareAddress(PeerAddress const& pa)
    : mIP{pa.ip.type() == IPv4
              ? asio::ip::address{asio::ip::address_v4{pa.ip.ipv4()}}
              : asio::ip::address_v6(pa.ip.ipv6())}
    , mPort{static_cast<unsigned short>(pa.port)}
    , mStringValue{formatAddress(mIP, mPort)}
{
}

PeerBareAddress
PeerBareAddress::resolve(std::string const& ipPort, Application& app,
                         unsigned short defaultPort)
{
    // (?:(v4addr)|(v6addr)|(hostname))(?:(:port))?
    static std::regex re(
        R"(^(?:(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})|(?:\[([[:xdigit:]:]+)\])|([[:alnum:].-]+)))"
        "(?:\\:(\\d{1,5}))?$");
    std::smatch m;

    if (!std::regex_search(ipPort, m, re) || m.empty())
    {
        throw std::runtime_error(
            fmt::format(FMT_STRING("Cannot parse peer address '{}'"), ipPort));
    }

    asio::ip::tcp::resolver::query::flags resolveflags;
    std::string toResolve;
    if (m[1].matched)
    {
        resolveflags = asio::ip::tcp::resolver::query::flags::numeric_host;
        toResolve = m[1].str();
    }
    else if (m[2].matched)
    {
        resolveflags = asio::ip::tcp::resolver::query::flags::numeric_host;
        toResolve = m[2].str();
    }
    else
    {
        resolveflags =
            asio::ip::tcp::resolver::query::flags::address_configured;
        toResolve = m[3].str();
    }

    asio::ip::tcp::resolver resolver(app.getWorkerIOContext());
    asio::ip::tcp::resolver::query query(toResolve, "", resolveflags);

    asio::error_code ec;
    asio::ip::tcp::resolver::iterator i = resolver.resolve(query, ec);
    if (ec)
    {
        LOG_DEBUG(DEFAULT_LOG, "Could not resolve '{}' : {}", ipPort,
                  ec.message());
        throw std::runtime_error(fmt::format(
            FMT_STRING("Could not resolve '{}': {}"), ipPort, ec.message()));
    }

    std::optional<asio::ip::address> ip;
    if (i != asio::ip::tcp::resolver::iterator())
    {
        asio::ip::tcp::endpoint end = *i;
        ip = end.address();
    }
    if (!ip.has_value())
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING("Could not resolve '{}': no IP addresses found"),
            ipPort));
    }

    unsigned short port = defaultPort;
    if (m[4].matched)
    {
        int parsedPort = atoi(m[4].str().c_str());
        if (parsedPort <= 0 || parsedPort > UINT16_MAX)
        {
            throw std::runtime_error(fmt::format(
                FMT_STRING("Could not resolve '{}': port out of range"),
                ipPort));
        }
        port = static_cast<unsigned short>(parsedPort);
    }

    releaseAssert(port != 0);

    return PeerBareAddress{*ip, port};
}

std::string const&
PeerBareAddress::toString() const
{
    return mStringValue;
}

namespace
{
template <uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t bits>
inline bool
ipv4InSubnet(unsigned long val)
{
    unsigned long constexpr subnet = (((static_cast<unsigned long>(a) << 24) |
                                       (static_cast<unsigned long>(b) << 16) |
                                       (static_cast<unsigned long>(c) << 8) |
                                       (static_cast<unsigned long>(d))));
    return (val & ~0UL << (32 - bits)) == subnet;
}

bool
isIPv4Private(asio::ip::address_v4 const& addr)
{
    // See RFC6890. Note: Table 10 (6to4 Relay Anycast) is global and Table 4
    // (Loopback) is handled in isLocalhost().
    unsigned long val = addr.to_ulong();
    return
        // 0.0.0.0/8 Table 1: This host on this network
        ipv4InSubnet<0, 0, 0, 0, 8>(val) ||
        // 10.0.0.0/8 Table 2: Private-Use Networks
        ipv4InSubnet<10, 0, 0, 0, 8>(val) ||
        // 100.64.0.0/10 Table 3: Shared Address Space
        ipv4InSubnet<100, 64, 0, 0, 10>(val) ||
        // 169.254.0.0/16 Table 5: Link Local
        ipv4InSubnet<169, 254, 0, 0, 16>(val) ||
        // 172.16.0.0/12 Table 6: Private-Use Networks
        ipv4InSubnet<172, 16, 0, 0, 12>(val) ||
        // 192.0.0.0/24 Table 7: IETF Protocol Assignments (and 192.0.0.0/29
        // Table 8: DS-Lite)
        ipv4InSubnet<192, 0, 0, 0, 24>(val) ||
        // 192.0.2.0/24 Table 9: TEST-NET-1
        ipv4InSubnet<192, 0, 2, 0, 24>(val) ||
        // 192.168.0.0/16 Table 11: Private-Use Networks
        ipv4InSubnet<192, 168, 0, 0, 16>(val) ||
        // 198.18.0.0/15 Table 12: Network Interconnect Device Benchmark
        // Testing
        ipv4InSubnet<198, 18, 0, 0, 15>(val) ||
        // 198.51.100.0/24 Table 13: TEST-NET-2
        ipv4InSubnet<198, 51, 100, 0, 24>(val) ||
        // 203.0.113.0/24 Table 14: TEST-NET-3
        ipv4InSubnet<203, 0, 113, 0, 24>(val) ||
        // 240.0.0.0/4 Table 15: Reserved for Future Use (and 255.255.255.255/32
        // Table 16: Limited Broadcast)
        ipv4InSubnet<240, 0, 0, 0, 4>(val);
}

template <uint8_t bits, uint8_t index, uint8_t currentByte, uint8_t... octets>
inline bool
ipv6InSubnetHelper(std::array<unsigned char, 16> const& bytes)
{
    if constexpr (bits == 0)
    {
        return true;
    }
    else if constexpr (bits >= 8)
    {
        if (bytes[index] != currentByte)
        {
            return false;
        }
        if constexpr (index < 15)
        {
            if constexpr (sizeof...(octets) > 0)
            {
                return ipv6InSubnetHelper<bits - 8, index + 1, octets...>(
                    bytes);
            }
            else
            {
                return ipv6InSubnetHelper<bits - 8, index + 1, 0>(bytes);
            }
        }
        else
        {
            return true;
        }
    }
    else
    {
        uint8_t constexpr mask = static_cast<uint8_t>(0xFF << (8 - bits));
        return (bytes[index] & mask) == (currentByte & mask);
    }
}

template <uint16_t a, uint16_t b, uint8_t bits>
constexpr bool
ipv6InSubnet(std::array<unsigned char, 16> const& bytes)
{
    return ipv6InSubnetHelper<bits, 0, (a >> 8), a & 0xFF, (b >> 8), b & 0xFF>(
        bytes);
}
} // namespace

bool
PeerBareAddress::isPrivate() const
{
    if (mIP.is_v4())
    {
        return isIPv4Private(mIP.to_v4());
    }

    auto v6 = mIP.to_v6();
    if (v6.is_v4_mapped() || v6.is_v4_compatible())
    {
        return isIPv4Private(v6.to_v4());
    }

    // See RFC6890. Note that it's okay if we return false on addresses that may
    // be private. So, since table 19 contains global addresses, table 22 has an
    // exception for more specific allocations, and table 27 is listed as N/A,
    // ignore them for now. Table 20 is handled by is_v4_mapped above.
    auto bytes = v6.to_bytes();
    return
        // ::/128 Table 18: Unspecified Address
        ipv6InSubnet<0, 0, 128>(bytes)
        // 100::/64 Table 21: Discard-Only Prefix
        || ipv6InSubnet<0x100, 0, 64>(bytes)
        // 2001::/32 Table 23: TEREDO
        || ipv6InSubnet<0x2001, 0, 32>(bytes)
        // 2001:2::/48 Table 24: Benchmarking
        || ipv6InSubnet<0x2001, 0x2, 48>(bytes)
        // 2001:db8::/32 Table 25: Documentation
        || ipv6InSubnet<0x2001, 0xdb8, 32>(bytes)
        // 2001:10::/28 Table 26: ORCHID
        || ipv6InSubnet<0x2001, 0x10, 28>(bytes)
        // fc00::/7 Table 28: Unique-Local
        || ipv6InSubnet<0xfc00, 0, 7>(bytes)
        // fe80::/10 Table 29: Linked-Scoped Unicast
        || ipv6InSubnet<0xfe80, 0, 10>(bytes);
}

bool
PeerBareAddress::isLocalhost() const
{
    return mIP.is_loopback();
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

bool
operator<(PeerBareAddress const& x, PeerBareAddress const& y)
{
    if (x.mPort < y.mPort)
    {
        return true;
    }
    if (x.mPort > y.mPort)
    {
        return false;
    }

    return x.mIP < y.mIP;
}
}
