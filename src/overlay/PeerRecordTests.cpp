#include "PeerRecord.h"
#include <soci.h>
#include "database/Database.h"
#include "lib/catch.hpp"
#include "main/Config.h"
#include "generated/StellarXDR.h"

namespace stellar
{
using namespace std;

TEST_CASE("toXdr", "[PeerRecord]")
{

    //PeerRecord pr 
    //PeerRecord::ipToXdr("1.2.3.4", actual);
}

//static bool parseIP(std::string ipStr, xdr::opaque_array<4U>& ret);
//static bool parseIPPort(const std::string& peerStr, int defaultPort, std::string& retIP, int& retPort);
//
//static bool loadPeerRecord(Database &db, string ip, int port, PeerRecord &ret);
//static void loadPeerRecords(Database &db, int max, VirtualClock::time_point nextAttemptCutoff, vector<PeerRecord>& retList);
//
//void storePeerRecord(Database& db);
//
//void backOff(VirtualClock &clock);
//static void createTable(Database &db);
//static const char *kSQLCreateStatement;

}