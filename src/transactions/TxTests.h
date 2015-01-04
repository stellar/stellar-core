#include "crypto/SecretKey.h"

namespace stellar
{
namespace txtest
{

extern SecretKey getRoot();

extern SecretKey getAccount(const char* n);

extern TransactionFramePtr setTrust(SecretKey& from, SecretKey& to, uint32_t seq, uint256& currencyCode);

extern TransactionFramePtr createPaymentTx(SecretKey& from, SecretKey& to, uint32_t seq, uint64_t amount);

extern TransactionFramePtr createCreditPaymentTx(SecretKey& from, SecretKey& to, CurrencyIssuer& ci, uint32_t seq, uint64_t amount);
}


}
