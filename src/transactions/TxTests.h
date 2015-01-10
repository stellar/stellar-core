#include "crypto/SecretKey.h"

namespace stellar
{
    class TransactionFrame;
    typedef shared_ptr<TransactionFrame> TransactionFramePtr;
namespace txtest
{

SecretKey getRoot();

SecretKey getAccount(const char* n);

TransactionFramePtr setTrust(SecretKey& from, SecretKey& to, uint32_t seq, const std::string& currencyCode);

TransactionFramePtr createPaymentTx(SecretKey& from, SecretKey& to, uint32_t seq, uint64_t amount);

TransactionFramePtr createCreditPaymentTx(SecretKey& from, SecretKey& to, Currency& ci, uint32_t seq,
    uint64_t amount);
}


}
