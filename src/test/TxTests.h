#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/LedgerCloseData.h"
#include "overlay/StellarXDR.h"
#include "util/optional.h"

namespace stellar
{
class AbstractLedgerTxn;
class ConstLedgerTxnEntry;
class TransactionFrame;
class OperationFrame;
class TxSetFrame;
class TestAccount;

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

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, time_t closeTime,
              std::vector<TransactionFrameBasePtr> const& txs = {},
              bool strictOrder = false);

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              std::vector<TransactionFrameBasePtr> const& txs = {},
              bool strictOrder = false);

SecretKey getRoot(Hash const& networkID);

SecretKey getAccount(std::string const& n);

Signer makeSigner(SecretKey key, int weight);

ConstLedgerTxnEntry loadAccount(AbstractLedgerTxn& ltx, PublicKey const& k,
                                bool mustExist = true);

bool doesAccountExist(Application& app, PublicKey const& k);

xdr::xvector<Signer, 20> getAccountSigners(PublicKey const& k,
                                           Application& app);

TransactionFramePtr
transactionFromOperationsV0(Application& app, SecretKey const& from,
                            SequenceNumber seq,
                            std::vector<Operation> const& ops, int fee = 0);
TransactionFramePtr
transactionFromOperationsV1(Application& app, SecretKey const& from,
                            SequenceNumber seq,
                            std::vector<Operation> const& ops, int fee = 0);
TransactionFramePtr transactionFromOperations(Application& app,
                                              SecretKey const& from,
                                              SequenceNumber seq,
                                              std::vector<Operation> const& ops,
                                              int fee = 0);

Operation changeTrust(Asset const& asset, int64_t limit);

Operation allowTrust(PublicKey const& trustor, Asset const& asset,
                     uint32_t authorize);

Operation inflation();

Operation accountMerge(PublicKey const& dest);

Operation manageData(std::string const& name, DataValue* value);

Operation bumpSequence(SequenceNumber to);

Operation createAccount(PublicKey const& dest, int64_t amount);

Operation payment(PublicKey const& to, int64_t amount);

Operation payment(PublicKey const& to, Asset const& asset, int64_t amount);

Operation createClaimableBalance(Asset const& asset, int64_t amount,
                                 xdr::xvector<Claimant, 10> const& claimants);

Operation claimClaimableBalance(ClaimableBalanceID const& balanceID);

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

Operation pathPaymentStrictSend(PublicKey const& to, Asset const& sendCur,
                                int64_t sendAmount, Asset const& destCur,
                                int64_t destMin,
                                std::vector<Asset> const& path);

Operation manageOffer(int64 offerId, Asset const& selling, Asset const& buying,
                      Price const& price, int64_t amount);
Operation manageBuyOffer(int64 offerId, Asset const& selling,
                         Asset const& buying, Price const& price,
                         int64_t amount);

Operation createPassiveOffer(Asset const& selling, Asset const& buying,
                             Price const& price, int64_t amount);

// returns the ID of the new offer if created
int64_t applyManageOffer(Application& app, int64 offerId,
                         SecretKey const& source, Asset const& selling,
                         Asset const& buying, Price const& price,
                         int64_t amount, SequenceNumber seq,
                         ManageOfferEffect expectedEffect);

int64_t applyManageBuyOffer(Application& app, int64 offerId,
                            SecretKey const& source, Asset const& selling,
                            Asset const& buying, Price const& price,
                            int64_t amount, SequenceNumber seq,
                            ManageOfferEffect expectedEffect);

// returns the ID of the new offer if created
int64_t applyCreatePassiveOffer(Application& app, SecretKey const& source,
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

Operation beginSponsoringFutureReserves(PublicKey const& sponsoredID);
Operation endSponsoringFutureReserves();
Operation revokeSponsorship(LedgerKey const& key);
Operation revokeSponsorship(AccountID const& accID, SignerKey const& key);

Operation clawback(AccountID const& from, Asset const& asset, int64_t amount);
Operation clawbackClaimableBalance(ClaimableBalanceID const& balanceID);

Asset makeNativeAsset();
Asset makeInvalidAsset();
Asset makeAsset(SecretKey const& issuer, std::string const& code);
Asset makeAssetAlphanum12(SecretKey const& issuer, std::string const& code);

OperationFrame const& getFirstOperationFrame(TransactionFrame const& tx);
OperationResult const& getFirstResult(TransactionFrame const& tx);
OperationResultCode getFirstResultCode(TransactionFrame const& tx);

// methods to check results based off meta data
void checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected);

void checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected,
             OperationResultCode code);

TransactionFrameBasePtr
transactionFrameFromOps(Hash const& networkID, TestAccount& source,
                        std::vector<Operation> const& ops,
                        std::vector<SecretKey> const& opKeys);
} // end txtest namespace
}
