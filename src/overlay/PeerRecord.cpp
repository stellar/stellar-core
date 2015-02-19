#include "PeerRecord.h"
#include <soci.h>
#include <vector>
#include "util/Logging.h"
#include "util/must_use.h"
#include "generated/StellarXDR.h"

#define SECONDS_PER_BACKOFF 10

namespace stellar
{

    using namespace std;
    using namespace soci;

// returns false if string is malformed
MUST_USE
bool PeerRecord::ipToXdr(string ip, xdr::opaque_array<4U>& ret)
{
    std::stringstream ss(ip);
    std::string item;
    int n = 0;
    while (std::getline(ss, item, '.') && n < 4)
    {
        ret[n] = atoi(item.c_str());
        n++;
    }
    if (n == 4)
        return true;

    return false;
}

MUST_USE
bool PeerRecord::toXdr(PeerAddress &ret)
{
    ret.port = mPort;
    ret.numFailures = mNumFailures;
    return ipToXdr(mIP, ret.ip);
}

MUST_USE
bool PeerRecord::fromIPPort(const string &ipPort, int defaultPort, VirtualClock &clock, PeerRecord &ret)
{
    string ip;
    int port;
    if (parseIPPort(ipPort, defaultPort, ip, port))
    {
        ret = PeerRecord { 0, ip, port, clock.now(), 0, 2 };
        return true;
    } else
        return false;
}

// LATER: verify ip and port are valid
MUST_USE
bool PeerRecord::parseIPPort(const std::string& peerStr, int defaultPort, std::string& retIP, int& retPort)
{
    std::string const innerStr(peerStr);
    std::string::const_iterator splitPoint =
        std::find(innerStr.rbegin(), innerStr.rend(), ':').base();
    if (splitPoint == innerStr.end())
    {
        retIP = innerStr;
        retPort = defaultPort;
    }
    else
    {
        splitPoint--;
        retIP.assign(innerStr.begin(), splitPoint);
        std::string portStr;
        splitPoint++;
        portStr.assign(splitPoint, innerStr.end());
        retPort = atoi(portStr.c_str());
        if (!retPort) return false;
    }
    return true;
}

MUST_USE
bool PeerRecord::loadPeerRecord(Database &db, string ip, int port, PeerRecord &ret)
{
    tm t;
    db.getSession() << "Select ip,port, nextAttempt, numFailures, Rank, FROM Peers WHERE ip = :v1 AND port = :v2",
        use(ip), use(port), into(ret.mIP), into(ret.mPort), into(t), into(ret.mNumFailures), into(ret.mRank);
    if (db.getSession().got_data())
    {
        ret.mNextAttempt = VirtualClock::tmToPoint(t);
        return true;
    } else
        return false;
}

void PeerRecord::loadPeerRecords(Database &db, int max, VirtualClock::time_point nextAttemptCutoff, vector<PeerRecord>& retList)
{
    try {
        rowset<row> rs =
            (db.getSession().prepare <<
            "SELECT peerID, ip, port, nextAttempt, numFailures, rank from Peers "
            " where nextAttempt < :nextAttempt "
            " order by rank limit :max ",
            use(VirtualClock::pointToTm(nextAttemptCutoff)), use(max));
        for (rowset<row>::const_iterator it = rs.begin(); it != rs.end(); ++it)
        {
            row const& row = *it;
            retList.push_back(PeerRecord(row.get<int>(0), row.get<std::string>(1), row.get<int>(2), 
                              VirtualClock::tmToPoint(row.get<tm>(3)),
                              row.get<int>(4), row.get<int>(5)));
        }
    }
    catch (soci_error& err)
    {
        LOG(ERROR) << "loadPeers Error: " << err.what();
    }
}

void PeerRecord::storePeerRecord(Database& db)
{
    try {
        int peerID;
        db.getSession() << "SELECT peerID from Peers where ip=:v1 and port=:v2",
            into(peerID), use(mIP), use(mPort);
        if (!db.getSession().got_data())
        {
            db.getSession() << "INSERT INTO Peers (IP,Port,nextAttempt,numFailures,Rank) values (:v1, :v2, :v3, :v4, :v5)",
                use(mIP), use(mPort), use(VirtualClock::pointToTm(mNextAttempt)), use(mNumFailures), use(mRank);
        }
        else
        {
            db.getSession() << "UPDATE Peers SET IP = :v1 and Port = :v2 and  nextAttempt = :v3, numFailures = :v4, Rank = :v5",
                use(mIP), use(mPort), use(VirtualClock::pointToTm(mNextAttempt)), use(mNumFailures), use(mRank);
        }
    }
    catch (soci_error& err)
    {
        LOG(ERROR) << "PeerRecord::storePeerRecord: " << err.what();
    }
}


void PeerRecord::backOff(VirtualClock &clock)
{
    mNumFailures++;

    mNextAttempt = clock.now() + std::chrono::seconds(
        static_cast<int64_t>(
        pow(2, mNumFailures) * SECONDS_PER_BACKOFF));


}

void PeerRecord::createTable(Database &db)
{
    if (db.isSqlite())
    {
        // Horrendous hack: replace "SERIAL" with "INTEGER" when
        // on SQLite:
        std::string q(kSQLCreateStatement);
        auto p = q.find("SERIAL");
        assert(p != std::string::npos);
        q.replace(p, 6, "INTEGER");
        db.getSession() << q.c_str();
    }
    else
    {
        db.getSession() << kSQLCreateStatement;
    }
}

const char* PeerRecord::kSQLCreateStatement = "CREATE TABLE IF NOT EXISTS Peers (						\
	peerID	SERIAL PRIMARY KEY,	\
    ip	    CHARACTER(11),		        \
    port   	INT DEFAULT 0 CHECK (port >= 0),		\
    nextAttempt   	TIMESTAMP,	    	\
    numFailures     INT DEFAULT 0 CHECK (numFailures >= 0),      \
    lastConnect   	TIMESTAMP,	    	\
	rank	INT DEFAULT 0 CHECK (rank >= 0)  	\
);";

}