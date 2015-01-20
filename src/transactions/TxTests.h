#include "crypto/SecretKey.h"
#include "transactions/TxResultCode.h"

namespace stellar
{
class TransactionFrame;
typedef std::shared_ptr<TransactionFrame> TransactionFramePtr;
namespace txtest
{

SecretKey getRoot();

SecretKey getAccount(const char* n);

TransactionFramePtr setTrust(SecretKey& from, SecretKey& to, uint32_t seq, const std::string& currencyCode);
void applyTrust(Application& app, SecretKey& from, SecretKey& to, 
    uint32_t seq, const std::string& currencyCode, TxResultCode result = txSUCCESS);

TransactionFramePtr createPaymentTx(SecretKey& from, SecretKey& to, uint32_t seq, uint64_t amount);
void applyPaymentTx(Application& app, SecretKey& from, SecretKey& to, 
    uint32_t seq, uint64_t amount, TxResultCode result = txSUCCESS);

TransactionFramePtr createCreditPaymentTx(SecretKey& from, SecretKey& to, Currency& ci, uint32_t seq,
    uint64_t amount);
void applyCreditPaymentTx(Application& app, SecretKey& from, SecretKey& to, Currency& ci, uint32_t seq,
    uint64_t amount, TxResultCode result = txSUCCESS);

TransactionFramePtr createOfferTx(SecretKey& source, Currency& takerGets, 
    Currency& takerPays, uint64_t price,uint64_t amount, uint32_t seq);

void applyOffer(Application& app, SecretKey& source, Currency& takerGets,
    Currency& takerPays, uint64_t price, uint64_t amount, uint32_t seq, TxResultCode result=txSUCCESS);

Currency makeCurrency(SecretKey& issuer, const std::string& code);

}  // end txtest namespace
}
