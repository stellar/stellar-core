// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerRecord.h"
#include <soci.h>
#include <vector>
#include <cmath>
#include "util/Logging.h"
#include "util/must_use.h"
#include "generated/StellarXDR.h"
#include "main/Application.h"
#include <regex>

#define SECONDS_PER_BACKOFF 10

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
PeerRecord::toXdr(PeerAddress& ret)
{
    ret.port = static_cast<unsigned short>(mPort);
    ret.numFailures = mNumFailures;
    ipToXdr(mIP, ret.ip);
}

void
PeerRecord::fromIPPort(string const& ip, unsigned short port,
                       VirtualClock& clock, PeerRecord& ret)
{
    ret = PeerRecord{ip, port, clock.now(), 0, 1};
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
        try
        {
            asio::ip::tcp::resolver::iterator i = resolver.resolve(query);
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
        }
        catch (asio::system_error&)
        {
            return false;
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

    ret = PeerRecord{ip, port, app.getClock().now(), 0, 1};
    return true;
}

optional<PeerRecord>
PeerRecord::loadPeerRecord(Database& db, string ip, unsigned short port)
{
    auto ret = make_optional<PeerRecord>();
    auto timer = db.getSelectTimer("peer");
    tm tm;
    db.getSession() << "Select ip,port, nextAttempt, numFailures, rank FROM "
                       "Peers WHERE ip = :v1 AND port = :v2",
        into(ret->mIP), into(ret->mPort), into(tm), into(ret->mNumFailures),
        into(ret->mRank), use(ip), use(uint32_t(port));
    if (db.getSession().got_data())
    {
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
        auto timer = db.getSelectTimer("peer");
        tm tm = VirtualClock::pointToTm(nextAttemptCutoff);
        PeerRecord pr;
        statement st =
            (db.getSession().prepare << "SELECT ip, port, nextAttempt, "
                                        "numFailures, rank from Peers "
                                        " where nextAttempt <= :nextAttempt "
                                        " order by rank limit :max ",
             use(tm), use(max), into(pr.mIP), into(pr.mPort), into(tm),
             into(pr.mNumFailures), into(pr.mRank));
        st.execute();
        while (st.fetch())
        {
            pr.mNextAttempt = VirtualClock::tmToPoint(tm);
            retList.push_back(pr);
        }
    }
    catch (soci_error& err)
    {
        LOG(ERROR) << "loadPeers Error: " << err.what();
    }
}

bool
PeerRecord::isPrivateAddress()
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
PeerRecord::isStored(Database& db)
{
    return loadPeerRecord(db, mIP, mPort) != nullopt<PeerRecord>();
}

void
PeerRecord::storePeerRecord(Database& db)
{
    try
    {
        auto tm = VirtualClock::pointToTm(mNextAttempt);
        statement stUp =
            (db.getSession().prepare << "UPDATE Peers SET "
                                        "nextAttempt=:v1,numFailures=:v2,Rank=:"
                                        "v3 WHERE IP=:v4 AND Port=:v5",
             use(tm), use(mNumFailures), use(mRank), use(mIP), use(mPort));
        {
            auto timer = db.getUpdateTimer("peer");
            stUp.execute(true);
        }
        if (stUp.get_affected_rows() != 1)
        {
            auto timer = db.getInsertTimer("peer");
            tm = VirtualClock::pointToTm(mNextAttempt);

            statement stIn =
                (db.getSession().prepare << "INSERT INTO Peers "
                                            "(IP,Port,nextAttempt,numFailures,"
                                            "Rank) values (:v1, :v2, :v3, :v4, "
                                            ":v5)",
                 use(mIP), use(mPort), use(tm), use(mNumFailures), use(mRank));

            stIn.execute(true);
            if (stIn.get_affected_rows() != 1)
                throw runtime_error("PeerRecord::storePeerRecord: failed on " +
                                    toString());
        }
    }
    catch (soci_error& err)
    {
        LOG(ERROR) << "PeerRecord::storePeerRecord: " << err.what();
    }
}

void
PeerRecord::backOff(VirtualClock& clock)
{
    mNumFailures++;

    mNextAttempt =
        clock.now() + std::chrono::seconds(static_cast<int64_t>(
                          std::pow(2, mNumFailures) * SECONDS_PER_BACKOFF));
}

string
PeerRecord::toString()
{
    return mIP + ":" + to_string(mPort);
}

void
PeerRecord::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS Peers;";
    db.getSession() << kSQLCreateStatement;
}

const char* PeerRecord::kSQLCreateStatement =
    "CREATE TABLE Peers ("
    "ip            VARCHAR(15) NOT NULL,"
    "port          INT DEFAULT 0 CHECK (port > 0 AND port <= 65535) NOT NULL,"
    "nextAttempt   TIMESTAMP NOT NULL,"
    "numFailures   INT DEFAULT 0 CHECK (numFailures >= 0) NOT NULL,"
    "rank          INT DEFAULT 0 CHECK (rank >= 0) NOT NULL,"
    "PRIMARY KEY (ip, port)"
    ");";
}
