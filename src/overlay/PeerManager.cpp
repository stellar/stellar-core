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
    if (x.mFlags != y.mFlags)
    {
        return false;
    }
    return x.mIsOutbound == y.mIsOutbound;
}

namespace PeerRecordModifiers
{

void
resetBackOff(Application& app, PeerRecord& peer)
{
    peer.mNumFailures = 0;
    auto nextAttempt = app.getClock().now();
    peer.mNextAttempt = VirtualClock::pointToTm(nextAttempt);
}

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

void
backOff(Application& app, PeerRecord& peer)
{
    peer.mNumFailures++;
    auto nextAttempt = app.getClock().now() + computeBackoff(peer.mNumFailures);
    peer.mNextAttempt = VirtualClock::pointToTm(nextAttempt);
}

void
markPreferred(Application&, PeerRecord& peer)
{
    peer.mFlags |= PEER_RECORD_FLAGS_PREFERRED;
    peer.mIsOutbound = true;
}

void
unmarkPreferred(Application&, PeerRecord& peer)
{
    peer.mFlags &= ~PEER_RECORD_FLAGS_PREFERRED;
}

void
markOutbound(Application&, PeerRecord& peer)
{
    peer.mIsOutbound = true;
}
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

PeerManager::PeerQuery
PeerManager::maxFailures(int maxFailures, bool outbound)
{
    return {false, maxFailures, outbound};
}

PeerManager::PeerQuery
PeerManager::nextAttemptCutoff(bool outbound)
{
    return {true, -1, outbound};
}

std::vector<PeerBareAddress>
PeerManager::getRandomPeers(PeerQuery const& query, size_t size,
                            std::function<bool(PeerBareAddress const&)> pred)
{
    auto& peers = mPeerCache[query];
    if (peers.size() < size)
    {
        peers = loadRandomPeers(query, size);
    }

    auto result = std::vector<PeerBareAddress>{};
    auto realSize = std::min(size, peers.size());

    auto it = std::begin(peers);
    auto end = std::end(peers);
    for (; it != end && result.size() < realSize; it++)
    {
        if (pred(*it))
        {
            result.push_back(*it);
        }
    }

    peers.erase(std::begin(peers), it);
    return result;
}

std::vector<PeerBareAddress>
PeerManager::loadRandomPeers(PeerQuery const& query, size_t size)
{
    // mBatchSize should always be bigger, so it should win anyway
    size = std::max(size, mBatchSize);

    // if we ever start removing peers from db, we may need to enable this
    // soci::transaction sqltx(mApp.getDatabase().getSession());
    // mApp.getDatabase().setCurrentTransactionReadOnly();

    std::string where;
    std::string clause = "WHERE";
    if (query.mNextAttempt)
    {
        where += clause + " nextattempt <= :nextattempt ";
        clause = "AND";
    }
    if (query.mMaxNumFailures >= 0)
    {
        where += clause + " numfailures <= :maxFailures ";
        clause = "AND";
    }
    if (query.mOutbound >= 0)
    {
        where += clause + " outbound = :outbound ";
    }

    std::tm nextAttempt = VirtualClock::pointToTm(mApp.getClock().now());
    int maxNumFailures = query.mMaxNumFailures;
    int outbound = query.mOutbound;

    auto bindToStatement = [&](soci::statement& st) {
        if (query.mNextAttempt)
        {
            st.exchange(soci::use(nextAttempt));
        }
        if (query.mMaxNumFailures >= 0)
        {
            st.exchange(soci::use(maxNumFailures));
        }
        if (query.mOutbound >= 0)
        {
            st.exchange(soci::use(outbound));
        }
    };

    auto result = std::vector<PeerBareAddress>{};
    auto count = countPeers(where, bindToStatement);
    if (count == 0)
    {
        return result;
    }

    auto maxOffset = count > size ? count - size : 0;
    auto offset = rand_uniform<size_t>(0, maxOffset);
    result = loadPeers(size, offset, where, bindToStatement);

    std::shuffle(std::begin(result), std::end(result), gRandomEngine);
    return result;
}

std::pair<PeerRecord, bool>
PeerManager::load(PeerBareAddress const& address)
{
    auto result = PeerRecord{};
    auto inDatabase = false;

    try
    {
        auto prep = mApp.getDatabase().getPreparedStatement(
            "SELECT numfailures, nextattempt, flags, outbound FROM peers "
            "WHERE ip = :v1 AND port = :v2");
        auto& st = prep.statement();
        st.exchange(into(result.mNumFailures));
        st.exchange(into(result.mNextAttempt));
        st.exchange(into(result.mFlags));
        st.exchange(into(result.mIsOutbound));
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
        LOG(ERROR) << "PeerManager::load error: " << err.what() << " on "
                   << address.toString();
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
                "flags = :v3, "
                "outbound = :v4 "
                "WHERE ip = :v5 AND port = :v6";
    }
    else
    {
        query = "INSERT INTO peers "
                "(nextattempt, numfailures, flags, outbound, ip,  port) "
                "VALUES "
                "(:v1,         :v2,         :v3,   :v4,      :v5, :v6)";
    }

    try
    {
        auto prep = mApp.getDatabase().getPreparedStatement(query);
        auto& st = prep.statement();
        st.exchange(use(peerRecord.mNextAttempt));
        st.exchange(use(peerRecord.mNumFailures));
        st.exchange(use(peerRecord.mFlags));
        st.exchange(use(peerRecord.mIsOutbound));
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
                LOG(ERROR) << "PeerManager::store failed on " +
                                  address.toString();
            }
        }
    }
    catch (soci_error& err)
    {
        LOG(ERROR) << "PeerManager::store error: " << err.what() << " on "
                   << address.toString();
    }
}

void
PeerManager::update(PeerBareAddress const& address,
                    std::vector<PeerRecordModifier> const& peerModifiers)
{
    auto peer = load(address);
    for (auto& peerModifier : peerModifiers)
    {
        peerModifier(mApp, peer.first);
    }
    store(address, peer.first, peer.second);
}

constexpr const auto BATCH_SIZE = 1000;

PeerManager::PeerManager(Application& app) : mApp(app), mBatchSize(BATCH_SIZE)
{
}

size_t
PeerManager::countPeers(std::string const& where,
                        std::function<void(soci::statement&)> const& bind)
{
    size_t count = 0;

    try
    {
        std::string sql = "SELECT COUNT(*) FROM peers " + where;

        auto prep = mApp.getDatabase().getPreparedStatement(sql);
        auto& st = prep.statement();

        bind(st);
        st.exchange(into(count));

        st.define_and_bind();
        st.execute(true);
    }
    catch (soci_error& err)
    {
        LOG(ERROR) << "countPeers error: " << err.what();
    }

    return count;
}

std::vector<PeerBareAddress>
PeerManager::loadPeers(int limit, int offset, std::string const& where,
                       std::function<void(soci::statement&)> const& bind)
{
    auto result = std::vector<PeerBareAddress>{};

    try
    {
        std::string sql = "SELECT ip, port "
                          "FROM peers " +
                          where + " LIMIT :limit OFFSET :offset";

        auto prep = mApp.getDatabase().getPreparedStatement(sql);
        auto& st = prep.statement();

        bind(st);
        st.exchange(use(limit));
        st.exchange(use(offset));

        std::string ip;
        int lport;
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
                result.emplace_back(ip, static_cast<unsigned short>(lport));
            }
            st.fetch();
        }
    }
    catch (soci_error& err)
    {
        LOG(ERROR) << "loadPeers error: " << err.what();
    }

    return result;
}

void
PeerManager::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS peers;";
    db.getSession() << kSQLCreateStatement;
}

bool
operator<(PeerManager::PeerQuery const& x, PeerManager::PeerQuery const& y)
{
    if (x.mNextAttempt < y.mNextAttempt)
    {
        return true;
    }
    if (x.mNextAttempt > y.mNextAttempt)
    {
        return false;
    }
    if (x.mMaxNumFailures < y.mMaxNumFailures)
    {
        return true;
    }
    if (x.mMaxNumFailures > y.mMaxNumFailures)
    {
        return false;
    }
    return x.mOutbound < y.mOutbound;
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
