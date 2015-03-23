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

SequenceNumber getAccountSeqNum(SecretKey const& k, Application& app);

uint64_t getAccountBalance(SecretKey const& k, Application& app);

TransactionFramePtr createChangeTrust(SecretKey& from, SecretKey& to,
                                      SequenceNumber seq,
                                      const std::string& currencyCode,
                                      int64_t limit);

void applyChangeTrust(Application& app, SecretKey& from, SecretKey& to,
                      SequenceNumber seq, const std::string& currencyCode,
                      int64_t limit,
                      ChangeTrustResultCode result = CHANGE_TRUST_SUCCESS);

TransactionFramePtr createAllowTrust(SecretKey& from, SecretKey& trustor,
                                     SequenceNumber seq,
                                     const std::string& currencyCode,
                                     bool authorize);

void applyAllowTrust(Application& app, SecretKey& from, SecretKey& trustor,
                     SequenceNumber seq, const std::string& currencyCode,
                     bool authorize,
                     AllowTrustResultCode result = ALLOW_TRUST_SUCCESS);

TransactionFramePtr createPaymentTx(SecretKey& from, SecretKey& to,
                                    SequenceNumber seq, int64_t amount);

void applyPaymentTx(Application& app, SecretKey& from, SecretKey& to,
                    SequenceNumber seq, int64_t amount,
                    PaymentResultCode result = PAYMENT_SUCCESS);

TransactionFramePtr createCreditPaymentTx(SecretKey& from, SecretKey& to,
                                          Currency& ci, SequenceNumber seq,
                                          int64_t amount);

void applyCreditPaymentTx(Application& app, SecretKey& from, SecretKey& to,
                          Currency& ci, SequenceNumber seq, int64_t amount,
                          PaymentResultCode result = PAYMENT_SUCCESS);

TransactionFramePtr createOfferOp(SecretKey& source, Currency& takerGets,
                                  Currency& takerPays, Price const& price,
                                  int64_t amount, SequenceNumber seq);

// expects success
// expects a new offer to be created
// returns the ID of the new offer
uint64_t applyCreateOffer(Application& app, LedgerDelta& delta,
                          SecretKey& source, Currency& takerGets,
                          Currency& takerPays, Price const& price,
                          int64_t amount, SequenceNumber seq);

CreateOfferResult
applyCreateOfferWithResult(Application& app, LedgerDelta& delta,
                           SecretKey& source, Currency& takerGets,
                           Currency& takerPays, Price const& price,
                           int64_t amount, SequenceNumber seq,
                           CreateOfferResultCode result = CREATE_OFFER_SUCCESS);

TransactionFramePtr createSetOptions(SecretKey& source,
                                     AccountID* inflationDest,
                                     uint32_t* setFlags, uint32_t* clearFlags,
                                     Thresholds* thrs, Signer* signer,
                                     SequenceNumber seq);

void applySetOptions(Application& app, SecretKey& source,
                     AccountID* inflationDest, uint32_t* setFlags,
                     uint32_t* clearFlags, Thresholds* thrs, Signer* signer,
                     SequenceNumber seq,
                     SetOptionsResultCode result = SET_OPTIONS_SUCCESS);

Currency makeCurrency(SecretKey& issuer, const std::string& code);

OperationFrame const& getFirstOperationFrame(TransactionFrame const& tx);
OperationResult const& getFirstResult(TransactionFrame const& tx);
OperationResultCode getFirstResultCode(TransactionFrame const& tx);

// modifying the type of the operation will lead to undefined behavior
Operation& getFirstOperation(TransactionFrame& tx);

} // end txtest namespace
}
