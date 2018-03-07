// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerRecord.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "overlay/StellarXDR.h"
#include "util/Logging.h"
#include "util/SociNoWarnings.h"
#include "util/must_use.h"
#include <algorithm>
#include <cmath>
#include <regex>
#include <vector>

#define SECONDS_PER_BACKOFF 10
#define MAX_BACKOFF_EXPONENT 10

namespace stellar
{

enum PeerRecordFlags
{
    PEER_RECORD_FLAGS_PREFERRED = 1
};

static const char* loadPeerRecordSelector =
    "SELECT ip, port, nextattempt, numfailures, flags FROM peers ";

using namespace std;
using namespace soci;

PeerRecord::PeerRecord(string const& ip, unsigned short port,
                       VirtualClock::time_point nextAttempt, int fails)
    : mIP(ip)
    , mPort(port)
    , mIsPreferred(false)
    , mNextAttempt(nextAttempt)
    , mNumFailures(fails)
{
    if (mIP.empty())
    {
        throw std::runtime_error("Cannot create PeerRecord with empty ip");
    }
    if (mPort == 0)
    {
        throw std::runtime_error("Cannot create PeerRecord with port 0");
    }
}

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

PeerRecord
PeerRecord::parseIPPort(string const& ipPort, Application& app,
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

    string ip;
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

    return PeerRecord{ip, port, app.getClock().now(), 0};
}

// peerRecordProcessor returns false if we should stop processing entries
void
PeerRecord::loadPeerRecords(
    Database& db, StatementContext& prep,
    std::function<bool(PeerRecord const&)> peerRecordProcessor)
{
    std::string ip;
    tm nextAttempt;
    int lport;
    int numFailures;
    auto& st = prep.statement();
    st.exchange(into(ip));
    st.exchange(into(lport));
    st.exchange(into(nextAttempt));
    st.exchange(into(numFailures));
    int flags;
    st.exchange(into(flags));

    st.define_and_bind();
    {
        auto timer = db.getSelectTimer("peer");
        st.execute(true);
    }
    while (st.got_data())
    {
        if (!ip.empty() && lport > 0)
        {
            auto pr =
                PeerRecord{ip, static_cast<unsigned short>(lport),
                           VirtualClock::tmToPoint(nextAttempt), numFailures};
            pr.setPreferred((flags & PEER_RECORD_FLAGS_PREFERRED) != 0);

            if (!peerRecordProcessor(pr))
            {
                return;
            }
        }
        st.fetch();
    }
}

optional<PeerRecord>
PeerRecord::loadPeerRecord(Database& db, string ip, unsigned short port)
{
    if (ip.empty() || port == 0)
    {
        return nullopt<PeerRecord>();
    }

    std::string sql = loadPeerRecordSelector;
    sql += "WHERE ip = :v1 AND port = :v2";

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();

    st.exchange(use(ip));
    int port32(port);
    st.exchange(use(port32));

    optional<PeerRecord> r;

    loadPeerRecords(db, prep, [&r](PeerRecord const& pr) {
        r = make_optional<PeerRecord>(pr);
        return false;
    });

    return r;
}

void
PeerRecord::loadPeerRecords(Database& db, int batchSize,
                            VirtualClock::time_point nextAttemptCutoff,
                            std::function<bool(PeerRecord const& pr)> pred)
{
    try
    {
        int offset = 0;
        bool didSomething;
        do
        {
            tm nextAttemptMax = VirtualClock::pointToTm(nextAttemptCutoff);

            std::string sql = loadPeerRecordSelector;
            sql += "WHERE nextattempt <= :nextattempt ORDER BY nextattempt "
                   "ASC, numfailures ASC LIMIT :max OFFSET :o";

            auto prep = db.getPreparedStatement(sql);
            auto& st = prep.statement();

            st.exchange(use(nextAttemptMax));
            st.exchange(use(batchSize));
            st.exchange(use(offset));

            didSomething = false;

            loadPeerRecords(db, prep, [&](PeerRecord const& pr) {
                offset++;
                didSomething = true;
                return pred(pr);
            });
        } while (didSomething);
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
PeerRecord::isPreferred() const
{
    return mIsPreferred;
}

void
PeerRecord::setPreferred(bool p)
{
    mIsPreferred = p;
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
            "( ip,  port, nextattempt, numfailures, flags) VALUES "
            "(:v1, :v2,  :v3,         :v4,          :v5)");
        auto& st = prep.statement();
        st.exchange(use(mIP));
        int port = mPort;
        st.exchange(use(port));
        st.exchange(use(tm));
        st.exchange(use(mNumFailures));
        int flags = (mIsPreferred ? PEER_RECORD_FLAGS_PREFERRED : 0);
        st.exchange(use(flags));

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
                                            "numfailures = :v2, "
                                            "flags = :v3 "
                                            "WHERE ip = :v4 AND port = :v5");
        auto& st = prep.statement();
        st.exchange(use(tm));
        st.exchange(use(mNumFailures));
        int flags = (mIsPreferred ? PEER_RECORD_FLAGS_PREFERRED : 0);
        st.exchange(use(flags));
        st.exchange(use(mIP));
        int port = mPort;
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
    mNextAttempt = mIsPreferred ? VirtualClock::time_point() : clock.now();
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
    int32 backoffCount = std::min<int32>(MAX_BACKOFF_EXPONENT, mNumFailures);

    auto nsecs = std::chrono::seconds(
        std::rand() % int(std::pow(2, backoffCount) * SECONDS_PER_BACKOFF) + 1);
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
