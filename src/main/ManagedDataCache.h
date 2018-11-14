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
    void update(Application& app);
    virtual std::string getAccount(Application& app) = 0;
    virtual void fulfill(std::vector<DataFrame::pointer> dfs) = 0;
  protected:
    int lcl = 0;
};
}
