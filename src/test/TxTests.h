#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/LedgerCloseData.h"
#include "overlay/StellarXDR.h"
#include <optional>

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

void checkLiquidityPool(Application& app, PoolID const& poolID,
                        int64_t reserveA, int64_t reserveB,
                        int64_t totalPoolShares,
                        int64_t poolSharesTrustLineCount);

TxSetResultMeta
closeLedger(Application& app,
            std::vector<TransactionFrameBasePtr> const& txs = {},
            bool strictOrder = false);

TxSetResultMeta
closeLedgerOn(Application& app, int day, int month, int year,
              std::vector<TransactionFrameBasePtr> const& txs = {},
              bool strictOrder = false);

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, TimePoint closeTime,
              std::vector<TransactionFrameBasePtr> const& txs = {},
              bool strictOrder = false);

TxSetResultMeta closeLedgerOn(Application& app, uint32 ledgerSeq,
                              time_t closeTime, TxSetFrameConstPtr txSet);

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              std::vector<TransactionFrameBasePtr> const& txs = {},
              bool strictOrder = false);

ConstLedgerTxnEntry loadAccount(AbstractLedgerTxn& ltx, PublicKey const& k,
                                bool mustExist = true);

bool doesAccountExist(Application& app, PublicKey const& k);

xdr::xvector<Signer, 20> getAccountSigners(PublicKey const& k,
                                           Application& app);

TransactionFrameBasePtr feeBump(Application& app, TestAccount& feeSource,
                                TransactionFrameBasePtr tx, int64_t fee);

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

ChangeTrustAsset makeChangeTrustAssetPoolShare(Asset const& assetA,
                                               Asset const& assetB,
                                               int32_t fee);

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
                        std::vector<SecretKey> const& opKeys,
                        std::optional<PreconditionsV2> cond = std::nullopt);

LedgerUpgrade makeBaseReserveUpgrade(int baseReserve);

LedgerHeader executeUpgrades(Application& app,
                             xdr::xvector<UpgradeType, 6> const& upgrades);

LedgerHeader executeUpgrade(Application& app, LedgerUpgrade const& lupgrade);

void
depositTradeWithdrawTest(Application& app, TestAccount& root, int depositSize,
                         std::vector<std::pair<bool, int64_t>> const& trades);

} // end txtest namespace
}
