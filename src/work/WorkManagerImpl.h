// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/WorkManager.h"

namespace stellar
{

class WorkManagerImpl : public WorkManager
{
  public:
    WorkManagerImpl(Application& app);
    virtual ~WorkManagerImpl();
    virtual void notify(std::string const&) override;
};
}
