#pragma once

#include "main/Application.h"
#include "ledger/DataFrame.h"

namespace stellar
{
// This class caches data stored on an account (manipulated via
//   the MANAGE_DATA op).
// The update() method should be called whenever a cache
//   instance is referenced.
// See WhiteList::instance() for an example.
class ManagedDataCache
{
  public:
    ManagedDataCache(Application& app) : mApp(app)
    {
        needsUpdate = true;
    }

    void setNeedsUpdate()
    {
        needsUpdate = true;
    }

    std::shared_ptr<AccountID> accountID();

    void update();
    virtual std::string getAccount() = 0;
    virtual void fulfill(std::vector<DataFrame::pointer> dfs) = 0;

  protected:
    Application& mApp;

  private:
    bool needsUpdate;
};
}
