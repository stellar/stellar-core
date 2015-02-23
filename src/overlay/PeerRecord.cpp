#include "overlay/PeerRecord.h"
#include <soci.h>
#include <vector>
#include <cmath>
#include "util/Logging.h"
#include "util/must_use.h"
#include "generated/StellarXDR.h"

#define SECONDS_PER_BACKOFF 10

namespace stellar
{

    using namespace std;
    using namespace soci;


void PeerRecord::ipToXdr(string ip, xdr::opaque_array<4U>& ret)
{
    stringstream ss(ip);
    string item;
    int n = 0;
    while (getline(ss, item, '.') && n < 4)
    {
        ret[n] = atoi(item.c_str());
        n++;
    }
    if (n != 4)
        throw runtime_error("PeerRecord::ipToXdr: failed on `" + ip + "`");
}

void PeerRecord::toXdr(PeerAddress &ret)
{
    ret.port = mPort;
    ret.numFailures = mNumFailures;
    ipToXdr(mIP, ret.ip);
}

void PeerRecord::fromIPPort(const string &ipPort, int defaultPort, VirtualClock &clock, PeerRecord &ret)
{
    string ip;
    int port;
    parseIPPort(ipPort, defaultPort, ip, port);
    ret = PeerRecord { 0, ip, port, clock.now(), 0, 1 };
}

// TODO: stricter verification that ip and port are valid
void PeerRecord::parseIPPort(const std::string& peerStr, int defaultPort, std::string& retIP, int& retPort)
{
    std::string const innerStr(peerStr);
    std::string::const_iterator splitPoint =
        std::find(innerStr.begin(), innerStr.end(), ':');
    if (splitPoint == innerStr.end())
    {
        retIP = innerStr;
        retPort = defaultPort;
    }
    else
    {
        retIP.assign(innerStr.begin(), splitPoint);
        std::string portStr;
        splitPoint++;
        portStr.assign(splitPoint, innerStr.end());
        retPort = atoi(portStr.c_str());
        if (!retPort) 
            throw runtime_error("PeerRecord::perseIPPort: failed on " + peerStr);
    }
}

MUST_USE
bool PeerRecord::loadPeerRecord(Database &db, string ip, int port, PeerRecord &ret)
{
    tm tm;
    db.getSession() << "Select ip,port, nextAttempt, numFailures, rank FROM Peers WHERE ip = :v1 AND port = :v2",
        into(ret.mIP), into(ret.mPort), into(tm), into(ret.mNumFailures), into(ret.mRank), use(ip), use(port);
    if (db.getSession().got_data())
    {
        ret.mNextAttempt = VirtualClock::tmToPoint(tm);
        return true;
    } else
        return false;
}

void PeerRecord::loadPeerRecords(Database &db, int max, VirtualClock::time_point nextAttemptCutoff, vector<PeerRecord>& retList)
{
    try {
        tm tm;
        PeerRecord pr;
        statement st = (db.getSession().prepare <<
            "SELECT ip, port, nextAttempt, numFailures, rank from Peers "
            " where nextAttempt < :nextAttempt "
            " order by rank limit :max ",
            use(tm), use(max), into(pr.mIP), into(tm), into(pr.mNumFailures), into(pr.mRank));
        st.execute();
        while(st.fetch())
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

bool PeerRecord::isStored(Database &db)
{
    PeerRecord pr;
    return loadPeerRecord(db, mIP, mPort, pr);
}

void PeerRecord::storePeerRecord(Database& db)
{
    try {
        //statement stUp = db.getSession().prepare << (
        //    "UPDATE Peers SET nextAttempt = " + to_string(VirtualClock::pointToTimeT(mNextAttempt)) + 
        //    " , numFailures = " + to_string(mNumFailures) + 
        //    ", Rank = " + to_string(mRank) + 
        //    " WHERE ip='" + mIP + "' AND port=" + to_string(mPort));
        auto tm = VirtualClock::pointToTm(mNextAttempt);
        statement stUp = (db.getSession().prepare <<
            "UPDATE Peers SET nextAttempt=:v1,numFailures=:v2,Rank=:v3 WHERE IP=:v4 AND Port=:v5",
            use(tm), use(mNumFailures), use(mRank), use(mIP), use(mPort));

        stUp.execute(true);
        if (stUp.get_affected_rows() != 1)
        {
            tm = VirtualClock::pointToTm(mNextAttempt);

            statement stIn = (db.getSession().prepare << "INSERT INTO Peers (IP,Port,nextAttempt,numFailures,Rank) values (:v1, :v2, :v3, :v4, :v5)",
                use(mIP), use(mPort), use(tm), use(mNumFailures), use(mRank));
            //auto tm = VirtualClock::pointToTimeT(mNextAttempt);
            //statement stInsert = db.getSession().prepare << (
            //    "INSERT INTO Peers (IP,Port,nextAttempt,numFailures,Rank) VALUES ('" +
            //    mIP + "', " + to_string(mPort) + ", " +
            //    to_string(tm) + ", " + to_string(mNumFailures) + ", " + to_string(mRank) + ");");
            stIn.execute(true);
            if (stIn.get_affected_rows() != 1)
                throw runtime_error("PeerRecord::storePeerRecord: failed on " + toString());

        }
    }
    catch (soci_error& err)
    {
        LOG(ERROR) << "PeerRecord::storePeerRecord: " << err.what();
    }
}


void 
PeerRecord::backOff(VirtualClock &clock)
{
    mNumFailures++;

    mNextAttempt = clock.now() + std::chrono::seconds(
        static_cast<int64_t>(
        std::pow(2, mNumFailures) * SECONDS_PER_BACKOFF));


}

string 
PeerRecord::toString()
{
    return mIP + ":" + to_string(mPort);
}

void 
PeerRecord::dropAll(Database &db)
{
    db.getSession() << "DROP TABLE IF EXISTS Peers;";
    db.getSession() << kSQLCreateStatement;
}

const char* PeerRecord::kSQLCreateStatement = "CREATE TABLE IF NOT EXISTS Peers (						\
    ip	    CHARACTER(11) NOT NULL,   \
    port   	INT DEFAULT 0 CHECK (port >= 0) NOT NULL,		\
    nextAttempt   	TIMESTAMP NOT NULL,	    	\
    numFailures     INT DEFAULT 0 CHECK (numFailures >= 0) NOT NULL,      \
    rank	INT DEFAULT 0 CHECK (rank >= 0) NOT NULL, 	\
    PRIMARY KEY (ip, port)                      \
);";

}
