#pragma once

#include "generated/StellarXDR.h"
#include "crypto/SecretKey.h"

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
    uint32_t seq, const std::string& currencyCode, ChangeTrust::ChangeTrustResultCode result = ChangeTrust::SUCCESS);

TransactionFramePtr createPaymentTx(SecretKey& from, SecretKey& to, uint32_t seq, uint64_t amount);
void applyPaymentTx(Application& app, SecretKey& from, SecretKey& to, 
    uint32_t seq, uint64_t amount, Payment::PaymentResultCode result = Payment::SUCCESS);

TransactionFramePtr createCreditPaymentTx(SecretKey& from, SecretKey& to, Currency& ci, uint32_t seq,
    uint64_t amount);
void applyCreditPaymentTx(Application& app, SecretKey& from, SecretKey& to, Currency& ci, uint32_t seq,
    uint64_t amount, Payment::PaymentResultCode result = Payment::SUCCESS);

TransactionFramePtr createOfferTx(SecretKey& source, Currency& takerGets, 
    Currency& takerPays, uint64_t price,uint64_t amount, uint32_t seq);

void applyOffer(Application& app, SecretKey& source, Currency& takerGets,
    Currency& takerPays, uint64_t price, uint64_t amount, uint32_t seq, CreateOffer::CreateOfferResultCode result=CreateOffer::SUCCESS);

Currency makeCurrency(SecretKey& issuer, const std::string& code);

}  // end txtest namespace
}
