#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/LedgerCloseData.h"
#include "ledger/AccountFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "overlay/StellarXDR.h"
#include "util/optional.h"

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
void applyTx(TransactionFramePtr const& tx, Application& app);

TxSetResultMeta closeLedgerOn(Application& app, uint32 ledgerSeq, int day,
                              int month, int year,
                              std::vector<TransactionFramePtr> const& txs = {});

SecretKey getRoot(Hash const& networkID);

SecretKey getAccount(const char* n);

// shorthand to load an existing account
AccountFrame::pointer loadAccount(SecretKey const& k, Application& app,
                                  bool mustExist = true);

// short hand to check that an account does not exist
void requireNoAccount(SecretKey const& k, Application& app);

OfferFrame::pointer loadOffer(PublicKey const& k, uint64 offerID,
                              Application& app, bool mustExist);

TrustFrame::pointer loadTrustLine(SecretKey const& k, Asset const& asset,
                                  Application& app, bool mustExist = true);

xdr::xvector<Signer, 20> getAccountSigners(SecretKey const& k,
                                           Application& app);

TransactionFramePtr transactionFromOperation(Hash const& networkID,
                                             SecretKey const& from,
                                             SequenceNumber seq,
                                             Operation const& op);
TransactionFramePtr
transactionFromOperations(Hash const& networkID, SecretKey const& from,
                          SequenceNumber seq,
                          std::vector<Operation> const& ops);

Operation
createChangeTrustOp(Asset const& asset, int64_t limit);

Operation
createAllowTrustOp(PublicKey const& trustor, Asset const& asset,
                   bool authorize);

Operation createInflationOp();

Operation createMergeOp(PublicKey const& dest);

Operation createManageDataOp(std::string const& name, DataValue* value);

Operation createCreateAccountOp(PublicKey const& dest,
                                int64_t amount);

Operation createPaymentOp(PublicKey const& to,
                          int64_t amount);

TransactionFramePtr createPaymentTx(Hash const& networkID,
                                    SecretKey const& from, PublicKey const& to,
                                    SequenceNumber seq, int64_t amount);

void applyPaymentTx(Application& app, SecretKey const& from,
                    SecretKey const& to, SequenceNumber seq, int64_t amount);

TransactionFramePtr createCreditPaymentTx(Hash const& networkID,
                                          SecretKey const& from,
                                          PublicKey const& to, Asset const& ci,
                                          SequenceNumber seq, int64_t amount);

void applyCreditPaymentTx(Application& app, SecretKey const& from,
                          PublicKey const& to, Asset const& ci,
                          SequenceNumber seq, int64_t amount);

TransactionFramePtr
createPathPaymentTx(Hash const& networkID, SecretKey const& from,
                    PublicKey const& to, Asset const& sendCur, int64_t sendMax,
                    Asset const& destCur, int64_t destAmount,
                    SequenceNumber seq, std::vector<Asset> const& path);

PathPaymentResult applyPathPaymentTx(Application& app, SecretKey const& from,
                                     PublicKey const& to, Asset const& sendCur,
                                     int64_t sendMax, Asset const& destCur,
                                     int64_t destAmount, SequenceNumber seq,
                                     std::vector<Asset> const& path,
                                     Asset* noIssuer = nullptr);

TransactionFramePtr manageOfferOp(Hash const& networkID, uint64 offerId,
                                  SecretKey const& source, Asset const& selling,
                                  Asset const& buying, Price const& price,
                                  int64_t amount, SequenceNumber seq);

TransactionFramePtr
createPassiveOfferOp(Hash const& networkID, SecretKey const& source,
                     Asset const& selling, Asset const& buying,
                     Price const& price, int64_t amount, SequenceNumber seq);

// returns the ID of the new offer if created
uint64_t applyManageOffer(Application& app, uint64 offerId,
                          SecretKey const& source, Asset const& selling,
                          Asset const& buying, Price const& price,
                          int64_t amount, SequenceNumber seq,
                          ManageOfferEffect expectedEffect);

// returns the ID of the new offer if created
uint64_t applyCreatePassiveOffer(Application& app, SecretKey const& source,
                                 Asset const& selling, Asset const& buying,
                                 Price const& price, int64_t amount,
                                 SequenceNumber seq,
                                 ManageOfferEffect expectedEffect);
Operation createSetOptionsOp(AccountID* inflationDest, uint32_t* setFlags,
                             uint32_t* clearFlags, ThresholdSetter* thrs,
                             Signer* signer, std::string* homeDomain);

Operation createSetOptionsOp(AccountID* inflationDest, uint32_t* setFlags,
                             uint32_t* clearFlags, ThresholdSetter* thrs,
                             Signer* signer, std::string* homeDomain);

Asset makeAsset(SecretKey const& issuer, std::string const& code);

OperationFrame const& getFirstOperationFrame(TransactionFrame const& tx);
OperationResult const& getFirstResult(TransactionFrame const& tx);
OperationResultCode getFirstResultCode(TransactionFrame const& tx);

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
