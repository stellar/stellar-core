#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "main/Application.h"
#include "test/TxTests.h"
#include "xdr/Stellar-types.h"
#include <vector>

namespace medida
{
class MetricsRegistry;
class Meter;
class Counter;
class Timer;
}

namespace stellar
{

class VirtualTimer;

class LoadGenerator
{
  public:
    LoadGenerator(Hash const& networkID);
    ~LoadGenerator();
    void clear();

    struct TxInfo;
    struct AccountInfo;
    using AccountInfoPtr = std::shared_ptr<AccountInfo>;

    static std::string pickRandomAsset();
    static const uint32_t STEP_MSECS;

    // Primary store of accounts.
    std::vector<AccountInfoPtr> mAccounts;

    // Subset of accounts that have issued credit in some asset.
    std::vector<AccountInfoPtr> mGateways;

    // Subset of accounts that have made offers to trade in some credits.
    std::vector<AccountInfoPtr> mMarketMakers;

    std::unique_ptr<VirtualTimer> mLoadTimer;
    int64 mMinBalance;
    uint64_t mLastSecond;

    // Schedule a callback to generateLoad() STEP_MSECS miliseconds from now.
    void scheduleLoadGeneration(Application& app, uint32_t nAccounts,
                                uint32_t nTxs, uint32_t txRate, bool autoRate);

    // Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
    // given target number of accounts and txs, and a given target tx/s rate.
    // If work remains after the current step, call scheduleLoadGeneration()
    // with the remainder.
    void generateLoad(Application& app, uint32_t nAccounts, uint32_t nTxs,
                      uint32_t txRate, bool autoRate);

    bool maybeCreateAccount(uint32_t ledgerNum, std::vector<TxInfo>& txs);

    std::vector<TxInfo> accountCreationTransactions(size_t n);
    AccountInfoPtr createAccount(size_t i, uint32_t ledgerNum = 0);
    std::vector<AccountInfoPtr> createAccounts(size_t n);
    bool loadAccount(Application& app, AccountInfo& account);
    bool loadAccount(Application& app, AccountInfoPtr account);
    bool loadAccounts(Application& app, std::vector<AccountInfoPtr> accounts);

    TxInfo createTransferNativeTransaction(AccountInfoPtr from,
                                           AccountInfoPtr to, int64_t amount);

    TxInfo
    createTransferCreditTransaction(AccountInfoPtr from, AccountInfoPtr to,
                                    int64_t amount,
                                    std::vector<AccountInfoPtr> const& path);

    AccountInfoPtr pickRandomAccount(AccountInfoPtr tryToAvoid,
                                     uint32_t ledgerNum);

    AccountInfoPtr pickRandomPath(AccountInfoPtr from, uint32_t ledgerNum,
                                  std::vector<AccountInfoPtr>& path);

    TxInfo createRandomTransaction(float alpha, uint32_t ledgerNum = 0);
    std::vector<TxInfo> createRandomTransactions(size_t n, float paretoAlpha);
    void updateMinBalance(Application& app);

    struct TrustLineInfo
    {
        AccountInfoPtr mIssuer;
        int64_t mBalance;
        int64_t mLimit;
    };

    struct AccountInfo : public std::enable_shared_from_this<AccountInfo>
    {
        AccountInfo(size_t id, SecretKey key, int64_t balance,
                    SequenceNumber seq, uint32_t lastChangedLedger,
                    LoadGenerator& loadGen);
        size_t mId;
        SecretKey mKey;
        int64_t mBalance;
        SequenceNumber mSeq;
        uint32_t mLastChangedLedger;

        void establishTrust(AccountInfoPtr a);
        bool canUseInLedger(uint32_t currentLedger);

        // Used when this account trusts some other account's credits.
        std::vector<TrustLineInfo> mTrustLines;

        // Asset issued, if a gateway, as well as reverse maps to
        // those accounts that trust this asset and those who are
        // buying and selling it.
        std::string mIssuedAsset;
        std::vector<AccountInfoPtr> mTrustingAccounts;
        std::vector<AccountInfoPtr> mBuyingAccounts;
        std::vector<AccountInfoPtr> mSellingAccounts;

        // Live offers, for accounts that are market makers.
        AccountInfoPtr mBuyCredit;
        AccountInfoPtr mSellCredit;

        void createDirectly(Application& app);
        void debitDirectly(Application& app, int64_t debitAmount);
        TxInfo creationTransaction();

      private:
        LoadGenerator& mLoadGen;
    };

    struct TxMetrics
    {
        medida::Meter& mAccountCreated;
        medida::Meter& mTrustlineCreated;
        medida::Meter& mOfferCreated;
        medida::Meter& mPayment;
        medida::Meter& mNativePayment;
        medida::Meter& mCreditPayment;
        medida::Meter& mOneOfferPathPayment;
        medida::Meter& mTwoOfferPathPayment;
        medida::Meter& mManyOfferPathPayment;
        medida::Meter& mTxnAttempted;
        medida::Meter& mTxnRejected;
        medida::Meter& mTxnBytes;

        medida::Counter& mGateways;
        medida::Counter& mMarketMakers;

        TxMetrics(medida::MetricsRegistry& m);
        void report();
    };

    struct TxInfo
    {
        AccountInfoPtr mFrom;
        AccountInfoPtr mTo;
        enum
        {
            TX_CREATE_ACCOUNT,
            TX_TRANSFER_NATIVE,
            TX_TRANSFER_CREDIT
        } mType;
        int64_t mAmount;
        std::vector<AccountInfoPtr> mPath;

        void touchAccounts(uint32_t ledger);
        bool execute(Application& app);

        void toTransactionFrames(Application& app,
                                 std::vector<TransactionFramePtr>& txs,
                                 TxMetrics& metrics);
        void recordExecution(int64_t baseFee);
    };
};
}
