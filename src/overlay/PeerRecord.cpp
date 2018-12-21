// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerRecord.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "overlay/StellarXDR.h"
#include "util/Logging.h"
#include "util/must_use.h"
#include <algorithm>
#include <cmath>
#include <regex>
#include <soci.h>
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

PeerRecord::PeerRecord(PeerBareAddress address,
                       VirtualClock::time_point nextAttempt, int fails)
    : mAddress(std::move(address))
    , mIsPreferred(false)
    , mNextAttempt(nextAttempt)
    , mNumFailures(fails)
{
    if (address.isEmpty())
    {
        throw std::runtime_error("Cannot create PeerRecord with empty address");
    }
}

void
PeerRecord::toXdr(PeerAddress& ret) const
{
    mAddress.toXdr(ret);
    ret.numFailures = mNumFailures;
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
            auto address =
                PeerBareAddress{ip, static_cast<unsigned short>(lport)};
            auto pr = PeerRecord{address, VirtualClock::tmToPoint(nextAttempt),
                                 numFailures};
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
PeerRecord::loadPeerRecord(Database& db, PeerBareAddress const& address)
{
    std::string sql = loadPeerRecordSelector;
    sql += "WHERE ip = :v1 AND port = :v2";

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();

    auto ip = address.getIP();
    st.exchange(use(ip));
    int port32(address.getPort());
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
        bool lastRes;
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

            lastRes = false;

            loadPeerRecords(db, prep, [&](PeerRecord const& pr) {
                offset++;
                lastRes = pred(pr);
                return lastRes;
            });
        } while (lastRes);
    }
    catch (soci_error& err)
    {
        LOG(ERROR) << "loadPeers Error: " << err.what();
    }
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

    auto other = loadPeerRecord(db, mAddress);

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
        auto ip = mAddress.getIP();
        st.exchange(use(ip));
        int port = mAddress.getPort();
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
        auto ip = mAddress.getIP();
        st.exchange(use(ip));
        int port = mAddress.getPort();
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

std::string
PeerRecord::toString() const
{
    return mAddress.toString();
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
    "flags         INT NOT NULL,"
    "PRIMARY KEY (ip, port)"
    ");";
}
