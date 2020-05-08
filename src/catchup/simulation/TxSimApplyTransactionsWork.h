// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "catchup/simulation/HistoryArchiveStream.h"
#include "ledger/LedgerTxn.h"
#include "work/ConditionalWork.h"
#include "work/Work.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
struct LedgerRange;

namespace txsimulation
{
class TxSimApplyTransactionsWork : public BasicWork
{
    TmpDir const& mDownloadDir;
    LedgerRange const mRange;
    Hash const mNetworkID;

    std::unique_ptr<HistoryArchiveStream> mStream;
    LedgerHeaderHistoryEntry mHeaderHistory;
    TransactionHistoryEntry mTransactionHistory;
    std::vector<TransactionEnvelope>::const_iterator mTransactionIter;
    TransactionHistoryResultEntry mResultHistory;
    std::vector<TransactionResultPair>::const_iterator mResultIter;

    uint32_t const mMaxOperations;
    // Force next version upgrade
    bool const mUpgradeProtocol;
    uint32_t const mMultiplier;
    bool const mVerifyResults;
    std::shared_ptr<ConditionalWork> mApplyLedgerWork;
    std::vector<TransactionResultPair> mResults;

    bool getNextLedgerFromHistoryArchive();

    bool getNextLedger(std::vector<TransactionEnvelope>& transactions,
                       std::vector<TransactionResultPair>& results,
                       std::vector<UpgradeType>& upgrades);

    size_t scaleLedger(std::vector<TransactionEnvelope>& transactions,
                       std::vector<TransactionResultPair>& results,
                       std::vector<UpgradeType>& upgrades, uint32_t partition);

    void addSignerKeys(AccountID const& acc, AbstractLedgerTxn& ltx,
                       std::set<SecretKey>& keys,
                       xdr::xvector<DecoratedSignature, 20> const& sigs,
                       uint32_t partition);

    void addSignerKeys(MuxedAccount const& acc, AbstractLedgerTxn& ltx,
                       std::set<SecretKey>& keys,
                       xdr::xvector<DecoratedSignature, 20> const& sigs,
                       uint32_t partition);

    void mutateTxSourceAccounts(TransactionEnvelope& env,
                                AbstractLedgerTxn& ltx,
                                std::set<SecretKey>& keys, uint32_t partition);
    void mutateOperations(TransactionEnvelope& env, AbstractLedgerTxn& ltx,
                          std::set<SecretKey>& keys, uint32_t partition);

  public:
    TxSimApplyTransactionsWork(Application& app, TmpDir const& downloadDir,
                               LedgerRange const& range,
                               std::string const& networkPassphrase,
                               uint32_t desiredOperations, bool upgrade,
                               uint32_t multiplier, bool verifyResults);
    void shutdown() override;

  protected:
    void onReset() override;
    State onRun() override;
    bool onAbort() override;
};
}
}
