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

TransactionFramePtr changeTrust(SecretKey& from, SecretKey& to, uint64_t submitTime,
    const std::string& currencyCode, int64_t limit);

void applyChangeTrust(Application& app, SecretKey& from, SecretKey& to, uint64_t submitTime,
    const std::string& currencyCode, int64_t limit, ChangeTrust::ChangeTrustResultCode result = ChangeTrust::SUCCESS);

TransactionFramePtr createPaymentTx(SecretKey& from, SecretKey& to, uint64_t submitTime, int64_t amount);

void applyPaymentTx(Application& app, SecretKey& from, SecretKey& to, uint64_t submitTime,
    int64_t amount, Payment::PaymentResultCode result = Payment::SUCCESS);

TransactionFramePtr createCreditPaymentTx(SecretKey& from, SecretKey& to, Currency& ci,
    uint64_t submitTime, int64_t amount);

void applyCreditPaymentTx(Application& app, SecretKey& from, SecretKey& to,
    Currency& ci, uint64_t submitTime, int64_t amount, Payment::PaymentResultCode result = Payment::SUCCESS);

TransactionFramePtr createOfferTx(SecretKey& source, Currency& takerGets, 
    Currency& takerPays, Price const& price, uint64_t submitTime,int64_t amount);

void applyOffer(Application& app, SecretKey& source, Currency& takerGets,
    Currency& takerPays, Price const& price, uint64_t submitTime, 
    int64_t amount, CreateOffer::CreateOfferResultCode result=CreateOffer::SUCCESS);

TransactionFramePtr createSetOptions(SecretKey& source, AccountID *inflationDest,
    uint32_t *setFlags, uint32_t *clearFlags, KeyValue *data,
    Thresholds *thrs, Signer *signer, uint64_t submitTime);

void applySetOptions(Application& app, SecretKey& source, AccountID *inflationDest,
    uint32_t *setFlags, uint32_t *clearFlags, KeyValue *data, Thresholds *thrs,
    Signer *signer, uint64_t submitTime, 
    SetOptions::SetOptionsResultCode result = SetOptions::SUCCESS);


Currency makeCurrency(SecretKey& issuer, const std::string& code);

}  // end txtest namespace
}
