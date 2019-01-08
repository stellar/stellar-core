#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/LedgerCloseData.h"
#include "overlay/StellarXDR.h"
#include "test/TestPrinter.h"
#include "util/optional.h"

namespace stellar
{
class AbstractLedgerTxn;
class ConstLedgerTxnEntry;
class TransactionFrame;
class OperationFrame;
class TxSetFrame;

namespace txtest
{

typedef std::vector<std::pair<TransactionResultPair, LedgerEntryChanges>>
    TxSetResultMeta;

struct ExpectedOpResult
{
    OperationResult mOperationResult;

    ExpectedOpResult(OperationResultCode code);
    ExpectedOpResult(CreateAccountResultCode createAccountCode);
    ExpectedOpResult(PaymentResultCode paymentCode);
    ExpectedOpResult(AccountMergeResultCode accountMergeCode);
    ExpectedOpResult(AccountMergeResultCode accountMergeCode,
                     int64_t sourceAccountBalance);
    ExpectedOpResult(SetOptionsResultCode setOptionsResultCode);
};

struct ValidationResult
{
    int64_t fee;
    TransactionResultCode code;
};

struct SetOptionsArguments
{
    optional<int> masterWeight;
    optional<int> lowThreshold;
    optional<int> medThreshold;
    optional<int> highThreshold;
    optional<Signer> signer;
    optional<uint32_t> setFlags;
    optional<uint32_t> clearFlags;
    optional<AccountID> inflationDest;
    optional<std::string> homeDomain;

    friend SetOptionsArguments operator|(SetOptionsArguments const& x,
                                         SetOptionsArguments const& y);
};

TransactionResult expectedResult(int64_t fee, size_t opsCount,
                                 TransactionResultCode code,
                                 std::vector<ExpectedOpResult> ops = {});

bool applyCheck(TransactionFramePtr tx, Application& app,
                bool checkSeqNum = true);
void applyTx(TransactionFramePtr const& tx, Application& app,
             bool checkSeqNum = true);
void validateTxResults(TransactionFramePtr const& tx, Application& app,
                       ValidationResult validationResult,
                       TransactionResult const& applyResult = {});

TxSetResultMeta closeLedgerOn(Application& app, uint32 ledgerSeq, int day,
                              int month, int year,
                              std::vector<TransactionFramePtr> const& txs = {});

SecretKey getRoot(Hash const& networkID);

SecretKey getAccount(const char* n);

Signer makeSigner(SecretKey key, int weight);

ConstLedgerTxnEntry loadAccount(AbstractLedgerTxn& ltx, PublicKey const& k,
                                bool mustExist = true);

bool doesAccountExist(Application& app, PublicKey const& k);

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
Operation setOptions(SetOptionsArguments const& arguments);

SetOptionsArguments setMasterWeight(int master);
SetOptionsArguments setLowThreshold(int low);
SetOptionsArguments setMedThreshold(int med);
SetOptionsArguments setHighThreshold(int high);
SetOptionsArguments setSigner(Signer signer);
SetOptionsArguments setFlags(uint32_t setFlags);
SetOptionsArguments clearFlags(uint32_t clearFlags);
SetOptionsArguments setInflationDestination(AccountID inflationDest);
SetOptionsArguments setHomeDomain(std::string const& homeDomain);

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
