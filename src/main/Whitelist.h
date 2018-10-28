#include "main/Application.h"

namespace stellar
{

class Whitelist
{
  public:
    Whitelist(Application& app);
    bool isWhitelisted(std::vector<DecoratedSignature> signatures,
                       Hash const& txHash);
    bool isWhitelistSig(DecoratedSignature const& sig, Hash const& txHash);

  private:
    std::unordered_map<uint32_t, std::vector<string64>> hash;
};
} // namespace stellar
