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
class TxSetFrame;

namespace txtest
{

typedef std::vector<std::pair<TransactionResultPair, LedgerEntryChanges>>
    TxSetResultMeta;

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

TxSetResultMeta closeLedgerOn(Application& app, uint32 ledgerSeq, int day,
                              int month, int year,
                              TransactionFramePtr tx = nullptr);

TxSetResultMeta closeLedgerOn(Application& app, uint32 ledgerSeq, int day,
                              int month, int year, TxSetFramePtr txSet);

void upgradeToCurrentLedgerVersion(Application& app);

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

int64_t getAccountBalance(SecretKey const& k, Application& app);

xdr::xvector<Signer,20> getAccountSigners(SecretKey const& k, Application& app);

TransactionFramePtr transactionFromOperation(Hash const& networkID, SecretKey const& from,
                                             SequenceNumber seq, Operation const& op);
TransactionFramePtr transactionFromOperations(Hash const& networkID, SecretKey const& from,
                                              SequenceNumber seq, const std::vector<Operation> &ops);

TransactionFramePtr createChangeTrust(Hash const& networkID, SecretKey const&from,
                                      PublicKey const&to, SequenceNumber seq,
                                      std::string const& assetCode,
                                      int64_t limit);

void applyChangeTrust(Application& app, SecretKey const&from, PublicKey const&to,
                      SequenceNumber seq, std::string const& assetCode,
                      int64_t limit);

TransactionFramePtr createAllowTrust(Hash const& networkID, SecretKey const&from,
                                     PublicKey const&trustor, SequenceNumber seq,
                                     std::string const& assetCode,
                                     bool authorize);

void applyAllowTrust(Application& app, SecretKey const&from, SecretKey const&trustor,
                     SequenceNumber seq, std::string const& assetCode,
                     bool authorize,
                     AllowTrustResultCode result = ALLOW_TRUST_SUCCESS);

TransactionFramePtr createCreateAccountTx(Hash const& networkID,
                                          SecretKey const& from, SecretKey const&to,
                                          SequenceNumber seq, int64_t amount);

void
applyCreateAccountTx(Application& app, SecretKey const&from, SecretKey const&to,
                     SequenceNumber seq, int64_t amount);

Operation createPaymentOp(SecretKey const* from, SecretKey const& to, int64_t amount);
TransactionFramePtr createPaymentTx(Hash const& networkID, SecretKey const&from,
                                    SecretKey const&to, SequenceNumber seq,
                                    int64_t amount);

void applyPaymentTx(Application& app, SecretKey const&from, SecretKey const&to,
                    SequenceNumber seq, int64_t amount,
                    PaymentResultCode result = PAYMENT_SUCCESS);

TransactionFramePtr createCreditPaymentTx(Hash const& networkID,
                                          SecretKey const&from, SecretKey const&to,
                                          Asset& ci, SequenceNumber seq,
                                          int64_t amount);

PaymentResult applyCreditPaymentTx(Application& app, SecretKey const&from,
                                   SecretKey const&to, Asset& ci, SequenceNumber seq,
                                   int64_t amount,
                                   PaymentResultCode result = PAYMENT_SUCCESS);

TransactionFramePtr createPathPaymentTx(Hash const& networkID, SecretKey const&from,
                                        SecretKey const&to, Asset const& sendCur,
                                        int64_t sendMax, Asset const& destCur,
                                        int64_t destAmount, SequenceNumber seq,
                                        std::vector<Asset>* path = nullptr);

PathPaymentResult
applyPathPaymentTx(Application& app, SecretKey const&from, SecretKey const&to,
                   Asset const& sendCur, int64_t sendMax, Asset const& destCur,
                   int64_t destAmount, SequenceNumber seq,
                   PathPaymentResultCode result = PATH_PAYMENT_SUCCESS,
                   std::vector<Asset>* path = nullptr);

TransactionFramePtr manageOfferOp(Hash const& networkID, uint64 offerId,
                                  SecretKey const&source, Asset const& selling,
                                  Asset const& buying, Price const& price,
                                  int64_t amount, SequenceNumber seq);

TransactionFramePtr createPassiveOfferOp(Hash const& networkID,
                                         SecretKey const&source, Asset& selling,
                                         Asset& buying, Price const& price,
                                         int64_t amount, SequenceNumber seq);

// returns the ID of the new offer if created
uint64_t applyManageOffer(Application& app, LedgerDelta& delta, uint64 offerId,
                          SecretKey const&source, Asset const& selling, Asset const& buying,
                          Price const& price, int64_t amount,
                          SequenceNumber seq, ManageOfferEffect expectedEffect);

TransactionFramePtr createSetOptions(Hash const& networkID, SecretKey const&source,
                                     SequenceNumber seq,
                                     AccountID* inflationDest,
                                     uint32_t* setFlags, uint32_t* clearFlags,
                                     ThresholdSetter* thrs, Signer* signer,
                                     std::string* homeDomain);

void applySetOptions(Application& app, SecretKey const&source, SequenceNumber seq,
                     AccountID* inflationDest, uint32_t* setFlags,
                     uint32_t* clearFlags, ThresholdSetter* thrs,
                     Signer* signer, std::string* homeDomain,
                     SetOptionsResultCode targetResult = SET_OPTIONS_SUCCESS);

TransactionFramePtr createInflation(Hash const& networkID, SecretKey const&from,
                                    SequenceNumber seq);
OperationResult applyInflation(Application& app, SecretKey const&from,
                               SequenceNumber seq,
                               InflationResultCode targetResult = INFLATION_SUCCESS);

TransactionFramePtr createAccountMerge(Hash const& networkID, SecretKey const&source,
                                       SecretKey const&dest, SequenceNumber seq);

void applyAccountMerge(Application& app, SecretKey const&source, SecretKey const&dest,
                       SequenceNumber seq,
                       AccountMergeResultCode targetResult = ACCOUNT_MERGE_SUCCESS);


TransactionFramePtr createManageData(Hash const& networkID, SecretKey const&source,
    std::string& name, DataValue* value,  SequenceNumber seq);

void applyManageData(
    Application& app, SecretKey const&source, std::string& name,
    DataValue* value,
    SequenceNumber seq, ManageDataResultCode targetResult = MANAGE_DATA_SUCCESS);

Asset makeAsset(SecretKey const&issuer, std::string const& code);

OperationFrame const& getFirstOperationFrame(TransactionFrame const& tx);
OperationResult const& getFirstResult(TransactionFrame const& tx);
OperationResultCode getFirstResultCode(TransactionFrame const& tx);

// modifying the type of the operation will lead to undefined behavior
Operation& getFirstOperation(TransactionFrame& tx);

void reSignTransaction(TransactionFrame& tx, SecretKey const&source);

// checks that b-maxd <= a <= b
// bias towards seller means
//    * amount left in an offer should be higher than the exact calculation
//    * amount received by a seller should be higher than the exact calculation
void checkAmounts(int64_t a, int64_t b, int64_t maxd = 1);

// methods to check results based off meta data
void checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected);

void checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected,
             OperationResultCode code);

} // end txtest namespace
}
