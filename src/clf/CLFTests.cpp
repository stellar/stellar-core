#include "main/Application.h"
#include "xdrpp/autocheck.h"
#include "clf/BucketList.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include <future>

using namespace stellar;

TEST_CASE("bucket list", "[clf]")
{

    Config const& cfg = getTestConfig();
    try
    {
        Application app(cfg);
        BucketList bl;
        autocheck::generator<std::vector<Bucket::KVPair>> gen;
        for (uint64_t i = 1; !app.getMainIOService().stopped() && i < 130; ++i)
        {
            app.getMainIOService().poll_one();
            bl.addBatch(app, i, gen(100));
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
