#include "main/Application.h"
#include "ledger/DataFrame.h"

namespace stellar
{

class ManagedObject
{
  public:
    void update(Application& app);
    virtual std::string getAccount(Application& app) = 0;
    virtual void fulfill(std::vector<DataFrame::pointer> dfs) = 0;
  protected:
    int lcl = 0;
};
}
