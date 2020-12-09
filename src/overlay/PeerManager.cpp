// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerManager.h"
#include "crypto/Random.h"
#include "database/Database.h"
#include "main/Application.h"
#include "overlay/RandomPeerSource.h"
#include "overlay/StellarXDR.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/must_use.h"

#include <Tracy.hpp>
#include <algorithm>
#include <cmath>
#include <fmt/format.h>
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
    if (VirtualClock::tmToSystemPoint(x.mNextAttempt) !=
        VirtualClock::tmToSystemPoint(y.mNextAttempt))
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

constexpr const auto BATCH_SIZE = 1000;
constexpr const auto MAX_FAILURES = 10;

PeerManager::PeerManager(Application& app)
    : mApp(app)
    , mOutboundPeersToSend(std::make_unique<RandomPeerSource>(
          *this, RandomPeerSource::maxFailures(MAX_FAILURES, true)))
    , mInboundPeersToSend(std::make_unique<RandomPeerSource>(
          *this, RandomPeerSource::maxFailures(MAX_FAILURES, false)))
{
}

std::vector<PeerBareAddress>
PeerManager::loadRandomPeers(PeerQuery const& query, int size)
{
    ZoneScoped;
    // BATCH_SIZE should always be bigger, so it should win anyway
    size = std::max(size, BATCH_SIZE);

    // if we ever start removing peers from db, we may need to enable this
    // soci::transaction sqltx(mApp.getDatabase().getSession());
    // mApp.getDatabase().setCurrentTransactionReadOnly();

    std::vector<std::string> conditions;
    if (query.mUseNextAttempt)
    {
        conditions.push_back("nextattempt <= :nextattempt");
    }
    if (query.mMaxNumFailures >= 0)
    {
        conditions.push_back("numfailures <= :maxFailures");
    }
    if (query.mTypeFilter == PeerTypeFilter::ANY_OUTBOUND)
    {
        conditions.push_back("type != :inboundType");
    }
    else
    {
        conditions.push_back("type = :type");
    }
    assert(!conditions.empty());
    std::string where = conditions[0];
    for (auto i = 1; i < conditions.size(); i++)
    {
        where += " AND " + conditions[i];
    }

    std::tm nextAttempt =
        VirtualClock::systemPointToTm(mApp.getClock().system_now());
    int maxNumFailures = query.mMaxNumFailures;
    int exactType = static_cast<int>(query.mTypeFilter);
    int inboundType = static_cast<int>(PeerType::INBOUND);

    auto bindToStatement = [&](soci::statement& st) {
        if (query.mUseNextAttempt)
        {
            st.exchange(soci::use(nextAttempt));
        }
        if (query.mMaxNumFailures >= 0)
        {
            st.exchange(soci::use(maxNumFailures));
        }
        if (query.mTypeFilter == PeerTypeFilter::ANY_OUTBOUND)
        {
            st.exchange(soci::use(inboundType));
        }
        else
        {
            st.exchange(soci::use(exactType));
        }
    };

    auto result = std::vector<PeerBareAddress>{};
    auto count = countPeers(where, bindToStatement);
    if (count == 0)
    {
        return result;
    }

    auto maxOffset = count > size ? count - size : 0;
    auto offset = rand_uniform<int>(0, maxOffset);
    result = loadPeers(size, offset, where, bindToStatement);

    std::shuffle(std::begin(result), std::end(result), gRandomEngine);
    return result;
}

void
PeerManager::removePeersWithManyFailures(int minNumFailures,
                                         PeerBareAddress const* address)
{
    ZoneScoped;
    try
    {
        auto& db = mApp.getDatabase();
        auto sql = std::string{
            "DELETE FROM peers WHERE numfailures >= :minNumFailures"};
        if (address)
        {
            sql += " AND ip = :ip";
        }

        auto prep = db.getPreparedStatement(sql);
        auto& st = prep.statement();

        st.exchange(use(minNumFailures));

        std::string ip;
        if (address)
        {
            ip = address->getIP();
            st.exchange(use(ip));
        }
        st.define_and_bind();

        {
            auto timer = db.getDeleteTimer("peer");
            st.execute(true);
        }
    }
    catch (soci_error& err)
    {
        CLOG_ERROR(Overlay,
                   "PeerManager::removePeersWithManyFailures error: {}",
                   err.what());
    }
}

std::vector<PeerBareAddress>
PeerManager::getPeersToSend(int size, PeerBareAddress const& address)
{
    ZoneScoped;
    auto keep = [&](PeerBareAddress const& pba) {
        return !pba.isPrivate() && pba != address;
    };

    auto peers = mOutboundPeersToSend->getRandomPeers(size, keep);
    if (peers.size() < size)
    {
        auto inbound = mInboundPeersToSend->getRandomPeers(
            size - static_cast<int>(peers.size()), keep);
        std::copy(std::begin(inbound), std::end(inbound),
                  std::back_inserter(peers));
    }

    return peers;
}

std::pair<PeerRecord, bool>
PeerManager::load(PeerBareAddress const& address)
{
    ZoneScoped;
    auto result = PeerRecord{};
    auto inDatabase = false;

    try
    {
        auto prep = mApp.getDatabase().getPreparedStatement(
            "SELECT numfailures, nextattempt, type FROM peers "
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
                    VirtualClock::systemPointToTm(mApp.getClock().system_now());
                result.mType = static_cast<int>(PeerType::INBOUND);
            }
        }
    }
    catch (soci_error& err)
    {
        CLOG_ERROR(Overlay, "PeerManager::load error: {} on {}", err.what(),
                   address.toString());
    }

    return std::make_pair(result, inDatabase);
}

void
PeerManager::store(PeerBareAddress const& address, PeerRecord const& peerRecord,
                   bool inDatabase)
{
    ZoneScoped;
    std::string query;

    if (inDatabase)
    {
        query = "UPDATE peers SET "
                "nextattempt = :v1, "
                "numfailures = :v2, "
                "type = :v3 "
                "WHERE ip = :v4 AND port = :v5";
    }
    else
    {
        query = "INSERT INTO peers "
                "(nextattempt, numfailures, type, ip,  port) "
                "VALUES "
                "(:v1,         :v2,        :v3,  :v4, :v5)";
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
                CLOG_ERROR(Overlay, "PeerManager::store failed on {}",
                           address.toString());
            }
        }
    }
    catch (soci_error& err)
    {
        CLOG_ERROR(Overlay, "PeerManager::store error: {} on {}", err.what(),
                   address.toString());
    }
}

void
PeerManager::update(PeerRecord& peer, TypeUpdate type)
{
    switch (type)
    {
    case TypeUpdate::ENSURE_OUTBOUND:
    {
        if (peer.mType == static_cast<int>(PeerType::INBOUND))
        {
            peer.mType = static_cast<int>(PeerType::OUTBOUND);
        }
        break;
    }
    case TypeUpdate::SET_PREFERRED:
    {
        peer.mType = static_cast<int>(PeerType::PREFERRED);
        break;
    }
    case TypeUpdate::ENSURE_NOT_PREFERRED:
    {
        if (peer.mType == static_cast<int>(PeerType::PREFERRED))
        {
            peer.mType = static_cast<int>(PeerType::OUTBOUND);
        }
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
    case BackOffUpdate::HARD_RESET:
    {
        peer.mNumFailures = 0;
        auto nextAttempt = app.getClock().system_now();
        peer.mNextAttempt = VirtualClock::systemPointToTm(nextAttempt);
        break;
    }
    case BackOffUpdate::RESET:
    case BackOffUpdate::INCREASE:
    {
        peer.mNumFailures =
            backOff == BackOffUpdate::RESET ? 0 : peer.mNumFailures + 1;
        auto nextAttempt =
            app.getClock().system_now() + computeBackoff(peer.mNumFailures);
        peer.mNextAttempt = VirtualClock::systemPointToTm(nextAttempt);
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
    ZoneScoped;
    auto peer = load(address);
    if (!peer.second)
    {
        CLOG_TRACE(Overlay, "Learned peer {} @{}", address.toString(),
                   mApp.getConfig().PEER_PORT);
        store(address, peer.first, peer.second);
    }
}

static PeerManager::TypeUpdate
getTypeUpdate(PeerRecord const& peer, PeerType observedType,
              bool preferredTypeKnown)
{
    PeerManager::TypeUpdate typeUpdate;
    bool isPreferredInDB = peer.mType == static_cast<int>(PeerType::PREFERRED);

    switch (observedType)
    {
    case PeerType::PREFERRED:
    {
        // Always update to preferred
        typeUpdate = PeerManager::TypeUpdate::SET_PREFERRED;
        break;
    }
    case PeerType::OUTBOUND:
    {
        if (isPreferredInDB && preferredTypeKnown)
        {
            // Downgrade to outbound if peer is definitely not preferred
            typeUpdate = PeerManager::TypeUpdate::ENSURE_NOT_PREFERRED;
        }
        else
        {
            // Maybe upgrade to outbound, or keep preferred
            typeUpdate = PeerManager::TypeUpdate::ENSURE_OUTBOUND;
        }
        break;
    }
    case PeerType::INBOUND:
    {
        // Either keep inbound type, or downgrade preferred to outbound
        typeUpdate = PeerManager::TypeUpdate::ENSURE_NOT_PREFERRED;
        break;
    }
    default:
    {
        abort();
    }
    }

    return typeUpdate;
}

void
PeerManager::update(PeerBareAddress const& address, PeerType observedType,
                    bool preferredTypeKnown)
{
    ZoneScoped;
    auto peer = load(address);
    TypeUpdate typeUpdate =
        getTypeUpdate(peer.first, observedType, preferredTypeKnown);
    update(peer.first, typeUpdate);
    store(address, peer.first, peer.second);
}

void
PeerManager::update(PeerBareAddress const& address, BackOffUpdate backOff)
{
    ZoneScoped;
    auto peer = load(address);
    update(peer.first, backOff, mApp);
    store(address, peer.first, peer.second);
}

void
PeerManager::update(PeerBareAddress const& address, PeerType observedType,
                    bool preferredTypeKnown, BackOffUpdate backOff)
{
    ZoneScoped;
    auto peer = load(address);
    TypeUpdate typeUpdate =
        getTypeUpdate(peer.first, observedType, preferredTypeKnown);
    update(peer.first, typeUpdate);
    update(peer.first, backOff, mApp);
    store(address, peer.first, peer.second);
}

int
PeerManager::countPeers(std::string const& where,
                        std::function<void(soci::statement&)> const& bind)
{
    ZoneScoped;
    int count = 0;

    try
    {
        std::string sql = "SELECT COUNT(*) FROM peers WHERE " + where;

        auto prep = mApp.getDatabase().getPreparedStatement(sql);
        auto& st = prep.statement();

        bind(st);
        st.exchange(into(count));

        st.define_and_bind();
        st.execute(true);
    }
    catch (soci_error& err)
    {
        CLOG_ERROR(Overlay, "countPeers error: {}", err.what());
    }

    return count;
}

std::vector<PeerBareAddress>
PeerManager::loadPeers(int limit, int offset, std::string const& where,
                       std::function<void(soci::statement&)> const& bind)
{
    ZoneScoped;
    auto result = std::vector<PeerBareAddress>{};

    try
    {
        std::string sql = "SELECT ip, port "
                          "FROM peers WHERE " +
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
        CLOG_ERROR(Overlay, "loadPeers error: {}", err.what());
    }

    return result;
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
    "type          INT NOT NULL,"
    "PRIMARY KEY (ip, port)"
    ");";
}
