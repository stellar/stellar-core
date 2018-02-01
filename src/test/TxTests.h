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
#include "test/TestPrinter.h"
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
    optional<int> masterWeight;
    optional<int> lowThreshold;
    optional<int> medThreshold;
    optional<int> highThreshold;
};

bool applyCheck(TransactionFramePtr tx, Application& app,
                bool checkSeqNum = true);
void applyTx(TransactionFramePtr const& tx, Application& app,
             bool checkSeqNum = true);

TxSetResultMeta closeLedgerOn(Application& app, uint32 ledgerSeq, int day,
                              int month, int year,
                              std::vector<TransactionFramePtr> const& txs = {});

SecretKey getRoot(Hash const& networkID);

SecretKey getAccount(const char* n);

// shorthand to load an existing account
AccountFrame::pointer loadAccount(PublicKey const& k, Application& app,
                                  bool mustExist = true);

// short hand to check that an account does not exist
void requireNoAccount(PublicKey const& k, Application& app);

OfferFrame::pointer loadOffer(PublicKey const& k, uint64 offerID,
                              Application& app, bool mustExist);

TrustFrame::pointer loadTrustLine(SecretKey const& k, Asset const& asset,
                                  Application& app, bool mustExist = true);

xdr::xvector<Signer, 20> getAccountSigners(PublicKey const& k,
                                           Application& app);

TransactionFramePtr
transactionFromOperations(Application& app, SecretKey const& from,
                          SequenceNumber seq,
                          std::vector<Operation> const& ops);

Operation changeTrust(Asset const& asset, int64_t limit);

Operation allowTrust(PublicKey const& trustor, Asset const& asset,
                     bool authorize);

Operation inflation();

Operation accountMerge(PublicKey const& dest);

Operation manageData(std::string const& name, DataValue* value);

Operation bumpSequence(SequenceNumber to);

Operation createAccount(PublicKey const& dest, int64_t amount);

Operation payment(PublicKey const& to, int64_t amount);

Operation payment(PublicKey const& to, Asset const& asset, int64_t amount);

TransactionFramePtr createPaymentTx(Application& app, SecretKey const& from,
                                    PublicKey const& to, SequenceNumber seq,
                                    int64_t amount);

TransactionFramePtr createCreditPaymentTx(Application& app,
                                          SecretKey const& from,
                                          PublicKey const& to, Asset const& ci,
                                          SequenceNumber seq, int64_t amount);

Operation pathPayment(PublicKey const& to, Asset const& sendCur,
                      int64_t sendMax, Asset const& destCur, int64_t destAmount,
                      std::vector<Asset> const& path);

Operation manageOffer(uint64 offerId, Asset const& selling, Asset const& buying,
                      Price const& price, int64_t amount);

Operation createPassiveOffer(Asset const& selling, Asset const& buying,
                             Price const& price, int64_t amount);

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

Operation setOptions(AccountID* inflationDest, uint32_t* setFlags,
                     uint32_t* clearFlags, ThresholdSetter* thrs,
                     Signer* signer, std::string* homeDomain);

Asset makeNativeAsset();
Asset makeInvalidAsset();
Asset makeAsset(SecretKey const& issuer, std::string const& code);

OperationFrame const& getFirstOperationFrame(TransactionFrame const& tx);
OperationResult const& getFirstResult(TransactionFrame const& tx);
OperationResultCode getFirstResultCode(TransactionFrame const& tx);

// methods to check results based off meta data
void checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected);

void checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected,
             OperationResultCode code);

} // end txtest namespace
}
