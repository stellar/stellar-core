#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "crypto/SecretKey.h"
#include "ledger/AccountFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "util/optional.h"
#include "herder/LedgerCloseData.h"

namespace stellar
{
class TransactionFrame;
class LedgerDelta;
class OperationFrame;
namespace txtest
{

struct ThresholdSetter
{
    optional<uint8_t> masterWeight;
    optional<uint8_t> lowThreshold;
    optional<uint8_t> medThreshold;
    optional<uint8_t> highThreshold;
};

bool applyCheck(TransactionFramePtr tx, LedgerDelta& delta, Application& app);

void checkEntry(LedgerEntry const& le, Application& app);
void checkAccount(AccountID const& id, Application& app);

time_t getTestDate(int day, int month, int year);

void closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month,
                   int year, TransactionFramePtr tx = nullptr);

SecretKey getRoot(Hash const& networkID);

SecretKey getAccount(const char* n);

// shorthand to load an existing account
AccountFrame::pointer loadAccount(SecretKey const& k, Application& app,
                                  bool mustExist = true);

// short hand to check that an account does not exist
void requireNoAccount(SecretKey const& k, Application& app);

OfferFrame::pointer loadOffer(SecretKey const& k, uint64 offerID,
                              Application& app, bool mustExist = true);

TrustFrame::pointer loadTrustLine(SecretKey const& k, Asset const& asset,
                                  Application& app, bool mustExist = true);

SequenceNumber getAccountSeqNum(SecretKey const& k, Application& app);

uint64_t getAccountBalance(SecretKey const& k, Application& app);

TransactionFramePtr createChangeTrust(Hash const& networkID, SecretKey& from,
                                      SecretKey& to, SequenceNumber seq,
                                      std::string const& assetCode,
                                      int64_t limit);

void applyChangeTrust(Application& app, SecretKey& from, SecretKey& to,
                      SequenceNumber seq, std::string const& assetCode,
                      int64_t limit,
                      ChangeTrustResultCode result = CHANGE_TRUST_SUCCESS);

TransactionFramePtr createAllowTrust(Hash const& networkID, SecretKey& from,
                                     SecretKey& trustor, SequenceNumber seq,
                                     std::string const& assetCode,
                                     bool authorize);

void applyAllowTrust(Application& app, SecretKey& from, SecretKey& trustor,
                     SequenceNumber seq, std::string const& assetCode,
                     bool authorize,
                     AllowTrustResultCode result = ALLOW_TRUST_SUCCESS);

TransactionFramePtr createCreateAccountTx(Hash const& networkID,
                                          SecretKey& from, SecretKey& to,
                                          SequenceNumber seq, int64_t amount);

void
applyCreateAccountTx(Application& app, SecretKey& from, SecretKey& to,
                     SequenceNumber seq, int64_t amount,
                     CreateAccountResultCode result = CREATE_ACCOUNT_SUCCESS);

TransactionFramePtr createPaymentTx(Hash const& networkID, SecretKey& from,
                                    SecretKey& to, SequenceNumber seq,
                                    int64_t amount);

void applyPaymentTx(Application& app, SecretKey& from, SecretKey& to,
                    SequenceNumber seq, int64_t amount,
                    PaymentResultCode result = PAYMENT_SUCCESS);

TransactionFramePtr createCreditPaymentTx(Hash const& networkID,
                                          SecretKey& from, SecretKey& to,
                                          Asset& ci, SequenceNumber seq,
                                          int64_t amount);

PaymentResult applyCreditPaymentTx(Application& app, SecretKey& from,
                                   SecretKey& to, Asset& ci, SequenceNumber seq,
                                   int64_t amount,
                                   PaymentResultCode result = PAYMENT_SUCCESS);

TransactionFramePtr createPathPaymentTx(Hash const& networkID, SecretKey& from,
                                        SecretKey& to, Asset const& sendCur,
                                        int64_t sendMax, Asset const& destCur,
                                        int64_t destAmount, SequenceNumber seq,
                                        std::vector<Asset>* path = nullptr);

PathPaymentResult
applyPathPaymentTx(Application& app, SecretKey& from, SecretKey& to,
                   Asset const& sendCur, int64_t sendMax, Asset const& destCur,
                   int64_t destAmount, SequenceNumber seq,
                   PathPaymentResultCode result = PATH_PAYMENT_SUCCESS,
                   std::vector<Asset>* path = nullptr);

TransactionFramePtr manageOfferOp(Hash const& networkID, uint64 offerId,
                                  SecretKey& source, Asset& selling,
                                  Asset& buying, Price const& price,
                                  int64_t amount, SequenceNumber seq);

TransactionFramePtr createPassiveOfferOp(Hash const& networkID,
                                         SecretKey& source, Asset& selling,
                                         Asset& buying, Price const& price,
                                         int64_t amount, SequenceNumber seq);

// expects success
// expects a new offer to be created
// returns the ID of the new offer
uint64_t applyCreateOffer(Application& app, LedgerDelta& delta, uint64 offerId,
                          SecretKey& source, Asset& selling, Asset& buying,
                          Price const& price, int64_t amount,
                          SequenceNumber seq);

ManageOfferResult applyCreateOfferWithResult(
    Application& app, LedgerDelta& delta, uint64 offerId, SecretKey& source,
    Asset& selling, Asset& buying, Price const& price, int64_t amount,
    SequenceNumber seq, ManageOfferResultCode result = MANAGE_OFFER_SUCCESS);

TransactionFramePtr createSetOptions(Hash const& networkID, SecretKey& source,
                                     SequenceNumber seq,
                                     AccountID* inflationDest,
                                     uint32_t* setFlags, uint32_t* clearFlags,
                                     ThresholdSetter* thrs, Signer* signer);

void applySetOptions(Application& app, SecretKey& source, SequenceNumber seq,
                     AccountID* inflationDest, uint32_t* setFlags,
                     uint32_t* clearFlags, ThresholdSetter* thrs,
                     Signer* signer,
                     SetOptionsResultCode result = SET_OPTIONS_SUCCESS);

TransactionFramePtr createInflation(Hash const& networkID, SecretKey& from,
                                    SequenceNumber seq);
OperationResult applyInflation(Application& app, SecretKey& from,
                               SequenceNumber seq,
                               InflationResultCode result = INFLATION_SUCCESS);

TransactionFramePtr createAccountMerge(Hash const& networkID, SecretKey& source,
                                       SecretKey& dest, SequenceNumber seq);

void applyAccountMerge(Application& app, SecretKey& source, SecretKey& dest,
                       SequenceNumber seq,
                       AccountMergeResultCode result = ACCOUNT_MERGE_SUCCESS);

Asset makeAsset(SecretKey& issuer, std::string const& code);

OperationFrame const& getFirstOperationFrame(TransactionFrame const& tx);
OperationResult const& getFirstResult(TransactionFrame const& tx);
OperationResultCode getFirstResultCode(TransactionFrame const& tx);

// modifying the type of the operation will lead to undefined behavior
Operation& getFirstOperation(TransactionFrame& tx);

void reSignTransaction(TransactionFrame& tx, SecretKey& source);

// checks that b-maxd <= a <= b
// bias towards seller means
//    * amount left in an offer should be higher than the exact calculation
//    * amount received by a seller should be higher than the exact calculation
void checkAmounts(int64_t a, int64_t b, int64_t maxd = 1);

} // end txtest namespace
}
