// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerRecord.h"
#include <vector>
#include <cmath>
#include "util/Logging.h"
#include "util/must_use.h"
#include "util/SociNoWarnings.h"
#include "overlay/StellarXDR.h"
#include "main/Application.h"
#include <regex>
#include <algorithm>

#define SECONDS_PER_BACKOFF 10
#define MAX_BACKOFF_EXPONENT 10

namespace stellar
{

using namespace std;
using namespace soci;

void
PeerRecord::ipToXdr(string ip, xdr::opaque_array<4U>& ret)
{
    stringstream ss(ip);
    string item;
    int n = 0;
    while (getline(ss, item, '.') && n < 4)
    {
        ret[n] = static_cast<unsigned char>(atoi(item.c_str()));
        n++;
    }
    if (n != 4)
        throw runtime_error("PeerRecord::ipToXdr: failed on `" + ip + "`");
}

void
PeerRecord::toXdr(PeerAddress& ret) const
{
    ret.port = mPort;
    ret.numFailures = mNumFailures;
    ipToXdr(mIP, ret.ip.ipv4());
}

bool
PeerRecord::parseIPPort(string const& ipPort, Application& app, PeerRecord& ret,
                        unsigned short defaultPort)
{
    static std::regex re(
        "^(?:(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3})|([[:alnum:].-]+))"
        "(?:\\:(\\d{1,5}))?$");
    std::smatch m;

    string ip;
    unsigned short port = defaultPort;

    if (std::regex_search(ipPort, m, re) && !m.empty())
    {
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
            LOG(DEBUG) << "Could not resolve '" << ipPort
                       << "' : " << ec.message();
            return false;
        }

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
            return false;

        if (m[3].matched)
        {
            int parsedPort = atoi(m[3].str().c_str());
            if (parsedPort <= 0 || parsedPort > UINT16_MAX)
                return false;
            port = static_cast<unsigned short>(parsedPort);
        }
    }
    else
    {
        return false;
    }

    if (port == 0)
        return false;

    ret = PeerRecord{ip, port, app.getClock().now(), 0};
    return true;
}

optional<PeerRecord>
PeerRecord::loadPeerRecord(Database& db, string ip, unsigned short port)
{
    auto ret = make_optional<PeerRecord>();
    tm tm;
    // SOCI only support signed short, using intermediate int avoids ending up
    // with negative numbers in the database
    uint32_t lport;
    auto prep =
        db.getPreparedStatement("SELECT ip,port, nextattempt, numfailures FROM "
                                "peers WHERE ip = :v1 AND port = :v2");
    auto& st = prep.statement();
    st.exchange(into(ret->mIP));
    st.exchange(into(lport));
    st.exchange(into(tm));
    st.exchange(into(ret->mNumFailures));
    st.exchange(use(ip));
    uint32_t port32(port);
    st.exchange(use(port32));
    st.define_and_bind();
    {
        auto timer = db.getSelectTimer("peer");
        st.execute(true);
    }
    if (st.got_data())
    {
        ret->mPort = static_cast<unsigned short>(lport);
        ret->mNextAttempt = VirtualClock::tmToPoint(tm);
        return ret;
    }
    else
        return nullopt<PeerRecord>();
}

void
PeerRecord::loadPeerRecords(Database& db, uint32_t max,
                            VirtualClock::time_point nextAttemptCutoff,
                            vector<PeerRecord>& retList)
{
    try
    {
        tm nextAttemptMax = VirtualClock::pointToTm(nextAttemptCutoff);
        PeerRecord pr;
        tm nextAttempt;
        uint32_t lport;
        auto prep = db.getPreparedStatement(
            "SELECT ip, port, nextattempt, numfailures "
            "FROM peers "
            "WHERE nextattempt <= :nextattempt "
            "ORDER BY nextattempt ASC, numfailures ASC limit :max ");
        auto& st = prep.statement();
        st.exchange(use(nextAttemptMax));
        st.exchange(use(max));
        st.exchange(into(pr.mIP));
        st.exchange(into(lport));
        st.exchange(into(nextAttempt));
        st.exchange(into(pr.mNumFailures));
        st.define_and_bind();
        {
            auto timer = db.getSelectTimer("peer");
            st.execute(true);
        }
        while (st.got_data())
        {
            pr.mPort = static_cast<unsigned short>(lport);
            pr.mNextAttempt = VirtualClock::tmToPoint(nextAttempt);
            retList.push_back(pr);
            st.fetch();
        }
    }
    catch (soci_error& err)
    {
        LOG(ERROR) << "loadPeers Error: " << err.what();
    }
}

bool
PeerRecord::isSelfAddressAndPort(std::string const& ip,
                                 unsigned short port) const
{
    asio::error_code ec;
    asio::ip::address_v4 addr = asio::ip::address_v4::from_string(mIP, ec);
    if (ec)
    {
        return false;
    }
    asio::ip::address_v4 otherAddr = asio::ip::address_v4::from_string(ip, ec);
    if (ec)
    {
        return false;
    }
    return (addr == otherAddr && port == mPort);
}

bool
PeerRecord::isPrivateAddress() const
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
PeerRecord::isLocalhost() const
{
    return mIP == "127.0.0.1";
}

bool
PeerRecord::isStored(Database& db)
{
    return loadPeerRecord(db, mIP, mPort) != nullopt<PeerRecord>();
}

bool
PeerRecord::insertIfNew(Database& db)
{
    auto tm = VirtualClock::pointToTm(mNextAttempt);

    auto other = loadPeerRecord(db, mIP, mPort);

    if (other)
    {
        return false;
    }
    else
    {
        auto prep = db.getPreparedStatement(
            "INSERT INTO peers "
            "( ip,  port, nextattempt, numfailures) VALUES "
            "(:v1, :v2,  :v3,         :v4)");
        auto& st = prep.statement();
        st.exchange(use(mIP));
        uint32_t port = uint32_t(mPort);
        st.exchange(use(port));
        st.exchange(use(tm));
        st.exchange(use(mNumFailures));
        st.define_and_bind();
        {
            auto timer = db.getInsertTimer("peer");
            st.execute(true);
        }
        return (st.get_affected_rows() == 1);
    }
}

void
PeerRecord::storePeerRecord(Database& db)
{
    if (!insertIfNew(db))
    {
        auto tm = VirtualClock::pointToTm(mNextAttempt);
        auto prep = db.getPreparedStatement("UPDATE peers SET "
                                            "nextattempt = :v1, "
                                            "numfailures = :v2 "
                                            "WHERE ip = :v3 AND port = :v4");
        auto& st = prep.statement();
        st.exchange(use(tm));
        st.exchange(use(mNumFailures));
        st.exchange(use(mIP));
        uint32_t port = uint32_t(mPort);
        st.exchange(use(port));
        st.define_and_bind();
        {
            auto timer = db.getUpdateTimer("peer");
            st.execute(true);
            if (st.get_affected_rows() != 1)
            {
                throw runtime_error("PeerRecord::storePeerRecord: failed on " +
                                    toString());
            }
        }
    }
}

void
PeerRecord::resetBackOff(VirtualClock& clock)
{
    mNumFailures = 0;
    mNextAttempt = clock.now();
    CLOG(DEBUG, "Overlay") << "PeerRecord: " << toString() << " backoff reset";
}

void
PeerRecord::backOff(VirtualClock& clock)
{
    mNumFailures++;

    auto nsecs = computeBackoff(clock);

    CLOG(DEBUG, "Overlay") << "PeerRecord: " << toString()
                           << " backoff, set nextAttempt at "
                           << "+" << nsecs.count() << " secs";
}

std::chrono::seconds
PeerRecord::computeBackoff(VirtualClock& clock)
{
    uint32 backoffCount = std::min<uint32>(MAX_BACKOFF_EXPONENT, mNumFailures);

    auto nsecs = std::chrono::seconds(
        std::rand() %
        (static_cast<uint32>(std::pow(2, backoffCount) * SECONDS_PER_BACKOFF)));
    mNextAttempt = clock.now() + nsecs;
    return nsecs;
}

string
PeerRecord::toString()
{
    return mIP + ":" + to_string(mPort);
}

void
PeerRecord::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS peers;";
    db.getSession() << kSQLCreateStatement;
}

const char* PeerRecord::kSQLCreateStatement =
    "CREATE TABLE peers ("
    "ip            VARCHAR(15) NOT NULL,"
    "port          INT DEFAULT 0 CHECK (port > 0 AND port <= 65535) NOT NULL,"
    "nextattempt   TIMESTAMP NOT NULL,"
    "numfailures   INT DEFAULT 0 CHECK (numfailures >= 0) NOT NULL,"
    "PRIMARY KEY (ip, port)"
    ");";
}
