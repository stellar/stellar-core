// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "main/Application.h"
#include "xdrpp/autocheck.h"
#include "clf/BucketList.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "util/Timer.h"
#include <future>

using namespace stellar;

TEST_CASE("bucket list", "[clf]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    try
    {
        Application app(clock, cfg);
        BucketList bl;
        autocheck::generator<std::vector<LedgerEntry>> gen;
        for (uint64_t i = 1; !app.getMainIOService().stopped() && i < 130; ++i)
        {
            app.crank(false);
            bl.addBatch(app, i, gen(10));
            // LOG(DEBUG) << "Finished addition cycle " << i;
            for (size_t j = 0; j < bl.numLevels(); ++j)
            {
                auto const& lev = bl.getLevel(j);
                auto currSz = lev.getCurr().getEntries().size();
                auto snapSz = lev.getSnap().getEntries().size();
                // LOG(DEBUG) << "level " << j
                //            << " curr=" << currSz
                //            << " snap=" << snapSz;
                CHECK(currSz <= BucketList::levelHalf(j) * 100);
                CHECK(snapSz <= BucketList::levelHalf(j) * 100);
            }
        }
    }
    catch (std::future_error& e)
    {
        LOG(DEBUG) << "Test caught std::future_error " << e.code() << ": "
                   << e.what();
    }
}
