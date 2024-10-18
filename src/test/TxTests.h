#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/LedgerCloseData.h"
#include "herder/Upgrades.h"
#include "overlay/StellarXDR.h"
#include "transactions/test/TransactionTestFrame.h"
#include <optional>

namespace stellar
{
class AbstractLedgerTxn;
class ConstLedgerTxnEntry;
class TransactionFrame;
class OperationFrame;
class TxSetXDRFrame;
class TestAccount;

namespace txtest
{

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
    std::optional<int> masterWeight;
    std::optional<int> lowThreshold;
    std::optional<int> medThreshold;
    std::optional<int> highThreshold;
    std::optional<Signer> signer;
    std::optional<uint32_t> setFlags;
    std::optional<uint32_t> clearFlags;
    std::optional<AccountID> inflationDest;
    std::optional<std::string> homeDomain;

    friend SetOptionsArguments operator|(SetOptionsArguments const& x,
                                         SetOptionsArguments const& y);
};

struct SetTrustLineFlagsArguments
{
    uint32_t setFlags = 0;
    uint32_t clearFlags = 0;
};

SetTrustLineFlagsArguments operator|(SetTrustLineFlagsArguments const& x,
                                     SetTrustLineFlagsArguments const& y);

TransactionResult expectedResult(int64_t fee, size_t opsCount,
                                 TransactionResultCode code,
                                 std::vector<ExpectedOpResult> ops = {});

bool applyCheck(TransactionTestFramePtr tx, Application& app,
                bool checkSeqNum = true);
void applyTx(TransactionTestFramePtr const& tx, Application& app,
             bool checkSeqNum = true);
void validateTxResults(TransactionTestFramePtr const& tx, Application& app,
                       ValidationResult validationResult,
                       TransactionResult const& applyResult = {});

void checkLiquidityPool(Application& app, PoolID const& poolID,
                        int64_t reserveA, int64_t reserveB,
                        int64_t totalPoolShares,
                        int64_t poolSharesTrustLineCount);

TransactionResultSet
closeLedger(Application& app,
            std::vector<TransactionFrameBasePtr> const& txs = {},
            bool strictOrder = false,
            xdr::xvector<UpgradeType, 6> const& upgrades = emptyUpgradeSteps);

TransactionResultSet
closeLedgerOn(Application& app, int day, int month, int year,
              std::vector<TransactionFrameBasePtr> const& txs = {},
              bool strictOrder = false);

TransactionResultSet
closeLedgerOn(Application& app, uint32 ledgerSeq, TimePoint closeTime,
              std::vector<TransactionFrameBasePtr> const& txs = {},
              bool strictOrder = false,
              xdr::xvector<UpgradeType, 6> const& upgrades = emptyUpgradeSteps);

TransactionResultSet closeLedger(Application& app, TxSetXDRFrameConstPtr txSet);

TransactionResultSet closeLedgerOn(Application& app, uint32 ledgerSeq,
                                   time_t closeTime,
                                   TxSetXDRFrameConstPtr txSet);

TransactionResultSet
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

TransactionTestFramePtr transactionFromOperationsV0(
    Application& app, SecretKey const& from, SequenceNumber seq,
    std::vector<Operation> const& ops, uint32_t fee = 0);
TransactionTestFramePtr
transactionFromOperationsV1(Application& app, SecretKey const& from,
                            SequenceNumber seq,
                            std::vector<Operation> const& ops, uint32_t fee,
                            std::optional<PreconditionsV2> cond = std::nullopt);
TransactionTestFramePtr
transactionFromOperations(Application& app, SecretKey const& from,
                          SequenceNumber seq, std::vector<Operation> const& ops,
                          uint32_t fee = 0);
TransactionTestFramePtr
transactionWithV2Precondition(Application& app, TestAccount& account,
                              int64_t sequenceDelta, uint32_t fee,
                              PreconditionsV2 const& cond);

// If useInclusionAsFullFee is true, `inclusion` will be used as the full fee.
// Otherwise, `tx` resource fee is added to full fee.
TransactionTestFramePtr feeBump(Application& app, TestAccount& feeSource,
                                std::shared_ptr<TransactionTestFrame const> tx,
                                int64_t inclusion,
                                bool useInclusionAsFullFee = false);

Operation changeTrust(Asset const& asset, int64_t limit);
Operation changeTrust(ChangeTrustAsset const& asset, int64_t limit);

Operation allowTrust(PublicKey const& trustor, Asset const& asset,
                     uint32_t authorize);

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

TransactionTestFramePtr createPaymentTx(Application& app, SecretKey const& from,
                                        PublicKey const& to, SequenceNumber seq,
                                        int64_t amount);

TransactionTestFramePtr
createCreditPaymentTx(Application& app, SecretKey const& from,
                      PublicKey const& to, Asset const& ci, SequenceNumber seq,
                      int64_t amount);

TransactionTestFramePtr createSimpleDexTx(Application& app,
                                          TestAccount& account, uint32 nbOps,
                                          uint32_t fee);

// Generates `UPLOAD_CONTRACT_WASM` host function operation with
// valid Wasm of *roughly* `generatedWasmSize` (within a few bytes).
// The output size deterministically depends on the input
// `generatedWasmSize`.
Operation createUploadWasmOperation(uint32_t generatedWasmSize);

TransactionTestFramePtr createUploadWasmTx(
    Application& app, TestAccount& account, uint32_t inclusionFee,
    int64_t resourceFee, SorobanResources resources,
    std::optional<std::string> memo = std::nullopt, int addInvalidOps = 0,
    std::optional<uint32_t> wasmSize = std::nullopt,
    std::optional<SequenceNumber> seq = std::nullopt);
int64_t sorobanResourceFee(Application& app, SorobanResources const& resources,
                           size_t txSize, uint32_t eventsSize);

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

Operation setTrustLineFlags(PublicKey const& trustor, Asset const& asset,
                            SetTrustLineFlagsArguments const& arguments);

SetTrustLineFlagsArguments setTrustLineFlags(uint32_t setFlags);
SetTrustLineFlagsArguments clearTrustLineFlags(uint32_t clearFlags);

Operation beginSponsoringFutureReserves(PublicKey const& sponsoredID);
Operation endSponsoringFutureReserves();
Operation revokeSponsorship(LedgerKey const& key);
Operation revokeSponsorship(AccountID const& accID, SignerKey const& key);

Operation clawback(AccountID const& from, Asset const& asset, int64_t amount);
Operation clawbackClaimableBalance(ClaimableBalanceID const& balanceID);

Operation liquidityPoolDeposit(PoolID const& poolID, int64_t maxAmountA,
                               int64_t maxAmountB, Price const& minPrice,
                               Price const& maxPrice);
Operation liquidityPoolWithdraw(PoolID const& poolID, int64_t amount,
                                int64_t minAmountA, int64_t minAmountB);

Asset makeNativeAsset();
Asset makeInvalidAsset();
Asset makeAsset(SecretKey const& issuer, std::string const& code);
Asset makeAssetAlphanum12(SecretKey const& issuer, std::string const& code);
ChangeTrustAsset makeChangeTrustAssetPoolShare(Asset const& assetA,
                                               Asset const& assetB,
                                               int32_t fee);

OperationResult const& getFirstResult(TransactionTestFramePtr tx);
OperationResultCode getFirstResultCode(TransactionTestFramePtr tx);

// methods to check results based off meta data
void checkTx(int index, TransactionResultSet& r,
             TransactionResultCode expected);

void checkTx(int index, TransactionResultSet& r, TransactionResultCode expected,
             OperationResultCode code);

TransactionTestFramePtr
transactionFrameFromOps(Hash const& networkID, TestAccount& source,
                        std::vector<Operation> const& ops,
                        std::vector<SecretKey> const& opKeys,
                        std::optional<PreconditionsV2> cond = std::nullopt);

TransactionTestFramePtr sorobanTransactionFrameFromOps(
    Hash const& networkID, TestAccount& source,
    std::vector<Operation> const& ops, std::vector<SecretKey> const& opKeys,
    SorobanResources const& resources, uint32_t inclusionFee,
    int64_t resourceFee, std::optional<std::string> memo = std::nullopt,
    std::optional<SequenceNumber> seq = std::nullopt);
TransactionTestFramePtr sorobanTransactionFrameFromOpsWithTotalFee(
    Hash const& networkID, TestAccount& source,
    std::vector<Operation> const& ops, std::vector<SecretKey> const& opKeys,
    SorobanResources const& resources, uint32_t totalFee, int64_t resourceFee,
    std::optional<std::string> memo = std::nullopt,
    std::optional<uint64> muxedData = std::nullopt);

ConfigUpgradeSetFrameConstPtr makeConfigUpgradeSet(
    AbstractLedgerTxn& ltx, ConfigUpgradeSet configUpgradeSet,
    bool expireSet = false,
    ContractDataDurability type = ContractDataDurability::TEMPORARY);
LedgerUpgrade makeConfigUpgrade(ConfigUpgradeSetFrame const& configUpgradeSet);

LedgerUpgrade makeBaseReserveUpgrade(int baseReserve);

LedgerHeader executeUpgrades(Application& app,
                             xdr::xvector<UpgradeType, 6> const& upgrades,
                             bool upgradesIgnored = false);

LedgerHeader executeUpgrade(Application& app, LedgerUpgrade const& lupgrade,
                            bool upgradeIgnored = false);

void
depositTradeWithdrawTest(Application& app, TestAccount& root, int depositSize,
                         std::vector<std::pair<bool, int64_t>> const& trades);

int64_t getBalance(Application& app, AccountID const& accountID,
                   Asset const& asset);

uint32_t getLclProtocolVersion(Application& app);

bool isSuccessResult(TransactionResult const& res);

} // end txtest namespace
}
