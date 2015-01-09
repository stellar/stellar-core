// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include <asio.hpp>
#include "main/Application.h"
#include "history/HistoryMaster.h"
#include "history/HistoryArchive.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "util/Timer.h"

#include <xdrpp/autocheck.h>

#ifdef _MSC_VER
#include <io.h>
#define UNLINK _unlink
#else
#include <unistd.h>
#define UNLINK unlink
#endif

using namespace stellar;

static void
del(std::string const& n)
{
    UNLINK(n.c_str());
}

TEST_CASE("WriteLedgerHistoryToFile", "[history]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application app(clock, cfg);
    autocheck::generator<History> gen;
    HistoryMaster hm(app);
    auto h1 = gen(10);
    auto fname = hm.writeLedgerHistoryToFile(h1);
    History h2;
    hm.readLedgerHistoryFromFile(fname, h2);
    CHECK(h1.fromLedger == h2.fromLedger);
    LOG(DEBUG) << "unlinking " << fname;
    del(fname);
}


TEST_CASE("HistoryArchiveParams::save", "[history]")
{
    HistoryArchiveParams hap;
    auto fname = "stellar-history.json";
    hap.save(fname);
    del(fname);
}
