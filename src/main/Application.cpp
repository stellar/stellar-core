// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Application.h"
#include "ApplicationImpl.h"

namespace stellar
{
using namespace std;

Application::pointer
Application::create(VirtualClock& clock, Config const& cfg, bool newDB)
{
    Application::pointer ret = make_shared<ApplicationImpl>(clock, cfg);
    if(newDB || cfg.DATABASE == "sqlite3://:memory:") ret->newDB();

    return ret;
}
}
