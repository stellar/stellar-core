#include "main/Application.h"

namespace stellar
{

class Whitelist
{
  public:
    Whitelist(Application& app);

    size_t unwhitelistedReserve(size_t setSize);

    bool isWhitelisted(std::vector<DecoratedSignature> signatures,
                       Hash const& txHash);
    bool isWhitelistSig(DecoratedSignature const& sig, Hash const& txHash);

  private:
    std::unordered_map<uint32_t, std::vector<string64>> hash;

	// default to a 5% reserve
	double reserve = 0.05;
};
} // namespace stellar
