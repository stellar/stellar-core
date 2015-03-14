#pragma once

#include "generated/StellarXDR.h"
#include "crypto/SecretKey.h"

namespace stellar
{
class TransactionFrame;
class LedgerDelta;
class OperationFrame;
typedef std::shared_ptr<TransactionFrame> TransactionFramePtr;
namespace txtest
{

SecretKey getRoot();

SecretKey getAccount(const char* n);

SequenceNumber getAccountSeqNum(SecretKey const& k, Application &app);

uint64_t getAccountBalance(SecretKey const& k, Application &app);

TransactionFramePtr createChangeTrust(SecretKey& from, SecretKey& to, SequenceNumber seq,
    const std::string& currencyCode, int64_t limit);

void applyChangeTrust(Application& app, SecretKey& from, SecretKey& to, SequenceNumber seq,
    const std::string& currencyCode, int64_t limit, ChangeTrust::ChangeTrustResultCode result = ChangeTrust::SUCCESS);

TransactionFramePtr createAllowTrust(SecretKey& from, SecretKey& trustor, SequenceNumber seq,
    const std::string& currencyCode, bool authorize);

void applyAllowTrust(Application& app, SecretKey& from, SecretKey& trustor, SequenceNumber seq,
    const std::string& currencyCode, bool authorize, AllowTrust::AllowTrustResultCode result = AllowTrust::SUCCESS);

TransactionFramePtr createPaymentTx(SecretKey& from, SecretKey& to, SequenceNumber seq, int64_t amount);

void applyPaymentTx(Application& app, SecretKey& from, SecretKey& to, SequenceNumber seq,
    int64_t amount, Payment::PaymentResultCode result = Payment::SUCCESS);

TransactionFramePtr createCreditPaymentTx(SecretKey& from, SecretKey& to, Currency& ci,
    SequenceNumber seq, int64_t amount);

void applyCreditPaymentTx(Application& app, SecretKey& from, SecretKey& to,
    Currency& ci, SequenceNumber seq, int64_t amount, Payment::PaymentResultCode result = Payment::SUCCESS);

TransactionFramePtr createOfferOp(SecretKey& source, Currency& takerGets, 
    Currency& takerPays, Price const& price,int64_t amount, SequenceNumber seq);

void applyCreateOffer(Application& app, LedgerDelta& delta, SecretKey& source, Currency& takerGets,
    Currency& takerPays, Price const& price, int64_t amount, SequenceNumber seq, CreateOffer::CreateOfferResultCode result=CreateOffer::SUCCESS);

TransactionFramePtr createSetOptions(SecretKey& source, AccountID *inflationDest,
    uint32_t *setFlags, uint32_t *clearFlags, KeyValue *data,
    Thresholds *thrs, Signer *signer, SequenceNumber seq);

void applySetOptions(Application& app, SecretKey& source, AccountID *inflationDest,
    uint32_t *setFlags, uint32_t *clearFlags, KeyValue *data, Thresholds *thrs,
    Signer *signer, SequenceNumber seq, SetOptions::SetOptionsResultCode result = SetOptions::SUCCESS);


Currency makeCurrency(SecretKey& issuer, const std::string& code);

OperationFrame& getFirstOperationFrame(TransactionFrame& tx);
OperationResult& getFirstResult(TransactionFrame& tx);
OperationResultCode getFirstResultCode(TransactionFrame& tx);

// modifying the type of the operation will lead to undefined behavior
Operation& getFirstOperation(TransactionFrame& tx);

}  // end txtest namespace
}
