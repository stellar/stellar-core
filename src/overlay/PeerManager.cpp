// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerManager.h"
#include "crypto/Random.h"
#include "database/Database.h"
#include "main/Application.h"
#include "overlay/StellarXDR.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/must_use.h"

#include <algorithm>
#include <cmath>
#include <lib/util/format.h>
#include <regex>
#include <soci.h>
#include <vector>

namespace stellar
{

using namespace soci;

enum PeerRecordFlags
{
    PEER_RECORD_FLAGS_PREFERRED = 1
};

bool
operator==(PeerRecord const& x, PeerRecord const& y)
{
    if (VirtualClock::tmToPoint(x.mNextAttempt) !=
        VirtualClock::tmToPoint(y.mNextAttempt))
    {
        return false;
    }
    if (x.mNumFailures != y.mNumFailures)
    {
        return false;
    }
    return x.mType == y.mType;
}

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

PeerAddress
toXdr(PeerBareAddress const& address)
{
    PeerAddress result;

    result.port = address.getPort();
    result.ip.type(IPv4);
    ipToXdr(address.getIP(), result.ip.ipv4());

    result.numFailures = 0;
    return result;
}

PeerManager::PeerManager(Application& app) : mApp(app)
{
}

std::pair<PeerRecord, bool>
PeerManager::load(PeerBareAddress const& address)
{
    auto result = PeerRecord{};
    auto inDatabase = false;

    try
    {
        auto prep = mApp.getDatabase().getPreparedStatement(
            "SELECT numfailures, nextattempt, flags FROM peers "
            "WHERE ip = :v1 AND port = :v2");
        auto& st = prep.statement();
        st.exchange(into(result.mNumFailures));
        st.exchange(into(result.mNextAttempt));
        st.exchange(into(result.mType));
        std::string ip = address.getIP();
        st.exchange(use(ip));
        int port = address.getPort();
        st.exchange(use(port));
        st.define_and_bind();
        {
            auto timer = mApp.getDatabase().getSelectTimer("peer");
            st.execute(true);
            inDatabase = st.got_data();

            if (!inDatabase)
            {
                result.mNextAttempt =
                    VirtualClock::pointToTm(mApp.getClock().now());
            }
        }
    }
    catch (soci_error& err)
    {
        CLOG(ERROR, "Overlay") << "PeerManager::load error: " << err.what()
                               << " on " << address.toString();
    }

    return std::make_pair(result, inDatabase);
}

void
PeerManager::store(PeerBareAddress const& address, PeerRecord const& peerRecord,
                   bool inDatabase)
{
    std::string query;

    if (inDatabase)
    {
        query = "UPDATE peers SET "
                "nextattempt = :v1, "
                "numfailures = :v2, "
                "flags = :v3 "
                "WHERE ip = :v4 AND port = :v5";
    }
    else
    {
        query = "INSERT INTO peers "
                "(nextattempt, numfailures, flags, ip,  port) "
                "VALUES "
                "(:v1,         :v2,         :v3,   :v4, :v5)";
    }

    try
    {
        auto prep = mApp.getDatabase().getPreparedStatement(query);
        auto& st = prep.statement();
        st.exchange(use(peerRecord.mNextAttempt));
        st.exchange(use(peerRecord.mNumFailures));
        st.exchange(use(peerRecord.mType));
        std::string ip = address.getIP();
        st.exchange(use(ip));
        int port = address.getPort();
        st.exchange(use(port));
        st.define_and_bind();
        {
            auto timer = mApp.getDatabase().getUpdateTimer("peer");
            st.execute(true);
            if (st.get_affected_rows() != 1)
            {
                CLOG(ERROR, "Overlay")
                    << "PeerManager::store failed on " + address.toString();
            }
        }
    }
    catch (soci_error& err)
    {
        CLOG(ERROR, "Overlay") << "PeerManager::store error: " << err.what()
                               << " on " << address.toString();
    }
}
void
PeerManager::update(PeerRecord& peer, TypeUpdate type)
{
    switch (type)
    {
    case TypeUpdate::KEEP:
    {
        break;
    }
    case TypeUpdate::SET_NORMAL:
    {
        peer.mType = static_cast<int>(PeerType::NORMAL);
        break;
    }
    case TypeUpdate::SET_PREFERRED:
    {
        peer.mType = static_cast<int>(PeerType::PREFERRED);
        break;
    }
    default:
    {
        abort();
    }
    }
}

namespace
{

static std::chrono::seconds
computeBackoff(int numFailures)
{
    constexpr const auto SECONDS_PER_BACKOFF = 10;
    constexpr const auto MAX_BACKOFF_EXPONENT = 10;

    auto backoffCount = std::min<int32_t>(MAX_BACKOFF_EXPONENT, numFailures);
    auto nsecs = std::chrono::seconds(
        std::rand() % int(std::pow(2, backoffCount) * SECONDS_PER_BACKOFF) + 1);
    return nsecs;
}
}

void
PeerManager::update(PeerRecord& peer, BackOffUpdate backOff, Application& app)
{
    switch (backOff)
    {
    case BackOffUpdate::KEEP:
    {
        break;
    }
    case BackOffUpdate::RESET:
    {
        peer.mNumFailures = 0;
        auto nextAttempt = app.getClock().now();
        peer.mNextAttempt = VirtualClock::pointToTm(nextAttempt);
        break;
    }
    case BackOffUpdate::INCREASE:
    {
        peer.mNumFailures++;
        auto nextAttempt =
            app.getClock().now() + computeBackoff(peer.mNumFailures);
        peer.mNextAttempt = VirtualClock::pointToTm(nextAttempt);
        break;
    }
    default:
    {
        abort();
    }
    }
}

void
PeerManager::ensureExists(PeerBareAddress const& address)
{
    auto peer = load(address);
    if (!peer.second)
    {
        store(address, peer.first, peer.second);
    }
}

void
PeerManager::update(PeerBareAddress const& address, TypeUpdate type)
{
    auto peer = load(address);
    update(peer.first, type);
    store(address, peer.first, peer.second);
}

void
PeerManager::update(PeerBareAddress const& address, BackOffUpdate backOff)
{
    auto peer = load(address);
    update(peer.first, backOff, mApp);
    store(address, peer.first, peer.second);
}

void
PeerManager::update(PeerBareAddress const& address, TypeUpdate type,
                    BackOffUpdate backOff)
{
    auto peer = load(address);
    update(peer.first, type);
    update(peer.first, backOff, mApp);
    store(address, peer.first, peer.second);
}

void
PeerManager::update(PeerBareAddress const& address,
                    std::chrono::seconds seconds)
{
    auto peer = load(address);
    peer.first.mNextAttempt =
        VirtualClock::pointToTm(mApp.getClock().now() + seconds);
    store(address, peer.first, peer.second);
}

void
PeerManager::loadPeers(int batchSize,
                       VirtualClock::time_point nextAttemptCutoff,
                       std::function<bool(PeerBareAddress const& address)> pred)
{
    try
    {
        int offset = 0;
        bool lastRes;
        do
        {
            tm nextAttemptMax = VirtualClock::pointToTm(nextAttemptCutoff);

            std::string sql = "SELECT ip, port FROM peers "
                              "WHERE nextattempt <= :nextattempt "
                              "ORDER BY nextattempt ASC, numfailures ASC "
                              "LIMIT :max OFFSET :o";

            auto prep = mApp.getDatabase().getPreparedStatement(sql);
            auto& st = prep.statement();

            st.exchange(use(nextAttemptMax));
            st.exchange(use(batchSize));
            st.exchange(use(offset));

            lastRes = false;

            loadPeers(prep, [&](PeerBareAddress const& address) {
                offset++;
                lastRes = pred(address);
                return lastRes;
            });
        } while (lastRes);
    }
    catch (soci_error& err)
    {
        CLOG(ERROR, "Overlay") << "loadPeers Error: " << err.what();
    }
}

void
PeerManager::loadPeers(
    StatementContext& prep,
    std::function<bool(PeerBareAddress const& address)> peerRecordProcessor)
{
    std::string ip;
    int lport;

    auto& st = prep.statement();
    st.exchange(into(ip));
    st.exchange(into(lport));

    st.define_and_bind();
    {
        auto timer = mApp.getDatabase().getSelectTimer("peer");
        st.execute(true);
    }
    while (st.got_data())
    {
        if (!ip.empty() && lport > 0)
        {
            auto address =
                PeerBareAddress{ip, static_cast<unsigned short>(lport)};
            if (!peerRecordProcessor(address))
            {
                return;
            }
        }
        st.fetch();
    }
}

void
PeerManager::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS peers;";
    db.getSession() << kSQLCreateStatement;
}

const char* PeerManager::kSQLCreateStatement =
    "CREATE TABLE peers ("
    "ip            VARCHAR(15) NOT NULL,"
    "port          INT DEFAULT 0 CHECK (port > 0 AND port <= 65535) NOT NULL,"
    "nextattempt   TIMESTAMP NOT NULL,"
    "numfailures   INT DEFAULT 0 CHECK (numfailures >= 0) NOT NULL,"
    "flags         INT NOT NULL,"
    "PRIMARY KEY (ip, port)"
    ");";
}
