// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "history/HistoryGateway.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"

#include <xdrpp/autocheck.h>

using namespace stellar;

TEST_CASE("WriteLedgerHistoryToFile", "[history]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application app(clock, cfg);
    autocheck::generator<stellarxdr::History> gen;
    HistoryMaster hm(app);
    auto h1 = gen(1000);
    auto fname = hm.writeLedgerHistoryToFile(h1);
    stellarxdr::History h2;
    hm.readLedgerHistoryFromFile(fname, h2);
    CHECK(h1.fromLedger == h2.fromLedger);
    LOG(DEBUG) << "unlinking " << fname;
#ifdef _MSC_VER
    _unlink(fname.c_str());
#else
    unlink(fname.c_str());
#endif
}
