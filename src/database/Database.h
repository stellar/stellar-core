#ifndef __DATABASE__
#define __DATABASE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <string>
#include <soci.h>

namespace stellar
{
class Application;

class Database
{
    Application& mApp;
    soci::session mSql;

    static bool gDriversRegistered;
    static void registerDrivers();

  public:
    Database(Application& app);

    soci::session &getSql()
    {
        return mSql;
    }
};
}

#endif
