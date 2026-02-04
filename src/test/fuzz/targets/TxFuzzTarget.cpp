// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/fuzz/targets/TxFuzzTarget.h"

#include "crypto/SHA.h"
#include "invariant/OrderBookIsNotCrossed.h"
#include "ledger/LedgerTxn.h"
#include "ledger/TrustLineWrapper.h"
#include "main/Application.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/fuzz/FuzzTargetRegistry.h"
#include "test/fuzz/FuzzUtils.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/OperationFrame.h"
#include "transactions/SignatureChecker.h"
#include "transactions/TransactionMeta.h"
#include "transactions/TransactionSQL.h"
#include "util/Logging.h"
#include "xdrpp/autocheck.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#ifdef BUILD_TESTS
#include "invariant/InvariantManager.h"
#endif // BUILD_TESTS

namespace stellar
{

namespace
{

auto constexpr FUZZER_MAX_OPERATIONS = 5;
auto constexpr INITIAL_ACCOUNT_BALANCE = 1'000'000LL;
auto constexpr INITIAL_ASSET_DISTRIBUTION = 1'000'000LL;
auto constexpr INITIAL_TRUST_LINE_LIMIT = 5 * INITIAL_ASSET_DISTRIBUTION;
auto constexpr DEFAULT_NUM_TRANSACTIONS_TO_RESERVE_FEES_FOR = 10;
auto constexpr MIN_ACCOUNT_BALANCE =
    FuzzUtils::FUZZING_FEE * DEFAULT_NUM_TRANSACTIONS_TO_RESERVE_FEES_FOR;

// must be strictly less than 255
uint8_t constexpr NUMBER_OF_PREGENERATED_ACCOUNTS = 5U;

auto constexpr NUMBER_OF_ASSETS_TO_USE = 5;

int64_t
getSequenceNumber(AbstractLedgerTxn& ltx, PublicKey const& sourceAccountID)
{
    auto account = stellar::loadAccount(ltx, sourceAccountID);
    return account.current().data.account().seqNum;
}

void
emplaceConditionallySponsored(xdr::xvector<Operation>& ops, Operation op,
                              bool isSponsored, int sponsorShortKey,
                              PublicKey const& sponsoredKey)
{
    if (isSponsored)
    {
        PublicKey sponsorKey;
        FuzzUtils::setShortKey(sponsorKey, sponsorShortKey);

        Operation beginSponsorOp;
        beginSponsorOp.body.type(BEGIN_SPONSORING_FUTURE_RESERVES);
        beginSponsorOp.body.beginSponsoringFutureReservesOp().sponsoredID =
            sponsoredKey;
        beginSponsorOp.sourceAccount.activate() = toMuxedAccount(sponsorKey);
        ops.emplace_back(beginSponsorOp);

        ops.emplace_back(op);

        Operation endSponsorOp;
        endSponsorOp.body.type(END_SPONSORING_FUTURE_RESERVES);
        endSponsorOp.sourceAccount.activate() = toMuxedAccount(sponsoredKey);
        ops.emplace_back(endSponsorOp);
    }
    else
    {
        ops.emplace_back(op);
    }
}

} // anonymous namespace

void
resetTxInternalState(Application& app)
{
    reinitializeAllGlobalStateForFuzzing(1);
#ifdef BUILD_TESTS
    app.getLedgerTxnRoot().resetForFuzzer();
    app.getInvariantManager().resetForFuzzer();
#endif // BUILD_TESTS
}

FuzzTransactionFrame::FuzzTransactionFrame(Hash const& networkID,
                                           TransactionEnvelope const& envelope)
    : TransactionFrame(networkID, envelope)
    , mTxResult(MutableTransactionResult::createSuccess(*this, 0))
{
}

void
FuzzTransactionFrame::attemptApplication(Application& app,
                                         AbstractLedgerTxn& ltx)
{
    // No soroban ops allowed
    if (std::any_of(getOperations().begin(), getOperations().end(),
                    [](auto const& x) { return x->isSoroban(); }))
    {
        mTxResult->setError(txFAILED);
        return;
    }

    // reset results of operations
    mTxResult = MutableTransactionResult::createSuccess(*this, 0);

    // attempt application of transaction without processing the fee or
    // committing the LedgerTxn.
    // Use AlwaysValidSignatureChecker to bypass signature validation. This is
    // necessary because:
    // 1. FuzzTransactionFrame creates transactions without real signatures
    // 2. When running as a standalone fuzz binary (with
    //    FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION), the regular
    //    SignatureChecker bypasses validation anyway
    // 3. When running smoke tests from the regular test binary (without
    //    FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION), we need explicit bypass
    AlwaysValidSignatureChecker signatureChecker{
        ltx.loadHeader().current().ledgerVersion, getContentsHash(),
        mEnvelope.v1().signatures};
    LedgerSnapshot ltxStmt(ltx);
    // if any ill-formed Operations, do not attempt transaction application
    auto isInvalidOperation = [&](auto const& op, auto& opResult) {
        auto diagnostics =
            DiagnosticEventManager::createForValidation(app.getConfig());
        return !op->checkValid(
            app.getAppConnector(), signatureChecker,
            &app.getAppConnector().getLastClosedSorobanNetworkConfig(), ltxStmt,
            false, opResult, diagnostics);
    };

    auto const& ops = getOperations();
    for (size_t i = 0; i < ops.size(); ++i)
    {
        auto const& op = ops[i];
        auto& opResult = mTxResult->getOpResultAt(i);
        if (isInvalidOperation(op, opResult))
        {
            mTxResult->setError(txFAILED);
            return;
        }
    }

    // while the following method's result is not captured, regardless, for
    // protocols < 8, this triggered buggy caching, and potentially may do
    // so in the future
    loadSourceAccount(ltx, ltx.loadHeader());
    processSeqNum(ltx);
    TransactionMetaBuilder tm(true, *this,
                              ltx.loadHeader().current().ledgerVersion,
                              app.getAppConnector());
    std::optional<SorobanNetworkConfig const> sorobanNetworkConfig;
    Hash sorobanRngSeed;
    applyOperations(signatureChecker, app.getAppConnector(), ltx, tm,
                    *mTxResult, sorobanNetworkConfig, sorobanRngSeed);
    if (mTxResult->getResultCode() == txINTERNAL_ERROR)
    {
        throw std::runtime_error("Internal error while fuzzing");
    }
}

TransactionResult const&
FuzzTransactionFrame::getResult() const
{
    return mTxResult->getXDR();
}

TransactionResultCode
FuzzTransactionFrame::getResultCode() const
{
    return mTxResult->getResultCode();
}

std::shared_ptr<FuzzTransactionFrame>
createFuzzTransactionFrame(AbstractLedgerTxn& ltx,
                           PublicKey const& sourceAccountID,
                           std::vector<Operation>::const_iterator begin,
                           std::vector<Operation>::const_iterator end,
                           Hash const& networkID)
{
    // construct a transaction envelope, which, for each transaction
    // application in the fuzzer, is the exact same, except for the inner
    // operations of course
    auto txEnv = TransactionEnvelope{};
    txEnv.type(ENVELOPE_TYPE_TX);
    auto& tx1 = txEnv.v1();
    tx1.tx.sourceAccount = toMuxedAccount(sourceAccountID);
    tx1.tx.fee = 0;
    tx1.tx.seqNum = getSequenceNumber(ltx, sourceAccountID) + 1;
    std::copy(begin, end, std::back_inserter(tx1.tx.operations));

    std::shared_ptr<FuzzTransactionFrame> res =
        std::make_shared<FuzzTransactionFrame>(networkID, txEnv);
    return res;
}

// Empties "ops" as operations are applied.  Throws if any operations fail.
// Handles breaking up the list of operations into multiple transactions, if the
// caller provides more operations than fit in a single transaction.
void
applySetupOperations(LedgerTxn& ltx, PublicKey const& sourceAccount,
                     xdr::xvector<Operation>::const_iterator begin,
                     xdr::xvector<Operation>::const_iterator end,
                     Application& app)
{
    while (begin != end)
    {
        auto endOpsInThisTx = std::distance(begin, end) <= MAX_OPS_PER_TX
                                  ? end
                                  : begin + MAX_OPS_PER_TX;
        auto txFramePtr = createFuzzTransactionFrame(
            ltx, sourceAccount, begin, endOpsInThisTx, app.getNetworkID());
        txFramePtr->attemptApplication(app, ltx);
        begin = endOpsInThisTx;

        if (txFramePtr->getResultCode() != txSUCCESS)
        {
            auto const msg =
                fmt::format(FMT_STRING("Error {} while setting up fuzzing -- "
                                       "{}"),
                            txFramePtr->getResultCode(),
                            xdrToCerealString(txFramePtr->getResult(),
                                              "TransactionResult"));
            LOG_FATAL(DEFAULT_LOG, "{}", msg);
            throw std::runtime_error(msg);
        }

        auto const& ops = txFramePtr->getOperations();
        for (size_t i = 0; i < ops.size(); ++i)
        {
            auto const& opFrame = ops.at(i);
            auto& opResult = txFramePtr->getResult().result.results().at(i);

            auto const& op = opFrame->getOperation();
            auto const& tr = opResult.tr();
            auto const opType = op.body.type();

            if ((opType == MANAGE_BUY_OFFER &&
                 tr.manageBuyOfferResult().success().offer.effect() !=
                     MANAGE_OFFER_CREATED) ||
                (opType == MANAGE_SELL_OFFER &&
                 tr.manageSellOfferResult().success().offer.effect() !=
                     MANAGE_OFFER_CREATED) ||
                (opType == CREATE_PASSIVE_SELL_OFFER &&
                 tr.createPassiveSellOfferResult().success().offer.effect() !=
                     MANAGE_OFFER_CREATED))
            {
                auto const msg = fmt::format(
                    FMT_STRING("Manage offer result {} while setting "
                               "up fuzzing -- {}"),
                    xdrToCerealString(tr, "Operation"),
                    xdrToCerealString(op, "Operation"));
                LOG_FATAL(DEFAULT_LOG, "{}", msg);
                throw std::runtime_error(msg);
            }
        }
    }
}

// Requires a set of operations small enough to fit in a single transaction.
// Tolerates the failure of transaction application.
void
applyFuzzOperations(LedgerTxn& ltx, PublicKey const& sourceAccount,
                    xdr::xvector<Operation>::const_iterator begin,
                    xdr::xvector<Operation>::const_iterator end,
                    Application& app)
{
    auto txFramePtr = createFuzzTransactionFrame(ltx, sourceAccount, begin, end,
                                                 app.getNetworkID());
    txFramePtr->attemptApplication(app, ltx);
}

namespace
{

// Unlike Asset, this can be a constexpr.
struct AssetID
{
    constexpr AssetID() : mIsNative(true), mIssuer(0), mSuffixDigit(0)
    {
    }

    constexpr AssetID(int id) : AssetID(id, id)
    {
    }

    constexpr AssetID(int issuer, int digit)
        : mIsNative(false), mIssuer(issuer), mSuffixDigit(digit)
    {
        assert(mSuffixDigit != 0); // id 0 is for native asset
        assert(mSuffixDigit < NUMBER_OF_ASSETS_TO_USE);
    }

    Asset
    toAsset() const
    {
        return mIsNative ? txtest::makeNativeAsset()
                         : FuzzUtils::makeAsset(mIssuer, mSuffixDigit);
    }

    bool const mIsNative;
    int const mIssuer;      // non-zero only if !isNative
    int const mSuffixDigit; // non-zero only if !isNative
};

struct SponsoredEntryParameters
{
    constexpr SponsoredEntryParameters() : SponsoredEntryParameters(false, 0)
    {
    }

    constexpr SponsoredEntryParameters(int sponsorKey)
        : SponsoredEntryParameters(true, sponsorKey)
    {
    }

    bool const mSponsored;
    int const mSponsorKey; // meaningful only if mSponsored is true

  private:
    constexpr SponsoredEntryParameters(bool sponsored, int sponsorKey)
        : mSponsored(sponsored), mSponsorKey(sponsorKey)
    {
    }
};

struct AccountParameters : public SponsoredEntryParameters
{
    constexpr AccountParameters(int shortKey,
                                int64_t nativeAssetAvailableForTestActivity,
                                uint32_t optionFlags)
        : SponsoredEntryParameters()
        , mShortKey(shortKey)
        , mNativeAssetAvailableForTestActivity(
              nativeAssetAvailableForTestActivity)
        , mOptionFlags(optionFlags)
    {
    }

    constexpr AccountParameters(int shortKey,
                                int64_t nativeAssetAvailableForTestActivity,
                                uint32_t optionFlags, int sponsorKey)
        : SponsoredEntryParameters(sponsorKey)
        , mShortKey(shortKey)
        , mNativeAssetAvailableForTestActivity(
              nativeAssetAvailableForTestActivity)
        , mOptionFlags(optionFlags)
    {
    }

    int const mShortKey;
    int64_t const mNativeAssetAvailableForTestActivity;
    uint32_t const mOptionFlags;
};

/*
Scenarios we are testing with the account, trustline, claimable balance, and
offer configurations below -
1. All possible account flags, along with issued assets.
2. Hitting limits due to buying liabilites for both native and non-native
   balances.
3. Claimable balances with claimants in all possible auth states and missing
   trustline.
4. Claimable balances with sponsor and issuer as the claimaint.
5. Order books for native to non-native, and non-native to non-native.
6. Offers created by the issuer.
7. Entries with sponsorships.
*/

std::array<AccountParameters,
           NUMBER_OF_PREGENERATED_ACCOUNTS> constexpr accountParameters{
    {// This account will have all of it's entries sponsored, and buying
     // liabilities close to INT64_MAX
     {0, 0, 0},
     {1, 256, AUTH_REVOCABLE_FLAG | AUTH_CLAWBACK_ENABLED_FLAG},
     // sponsored by account 1 and AUTH_REVOCABLE so we can put a trustline
     // into the AUTHORIZED_TO_MAINTAIN_LIABILITIES state
     {2, 256, AUTH_REVOCABLE_FLAG, 1},
     {3, 256, AUTH_REQUIRED_FLAG},
     {4, 256, AUTH_IMMUTABLE_FLAG}}};

struct TrustLineParameters : public SponsoredEntryParameters
{
    constexpr TrustLineParameters(int trustor, AssetID const& assetID,
                                  int64_t assetAvailableForTestActivity,
                                  int64_t spareLimitAfterSetup)
        : TrustLineParameters(trustor, assetID, assetAvailableForTestActivity,
                              spareLimitAfterSetup, false, 0)
    {
    }

    static TrustLineParameters constexpr withAllowTrust(
        int trustor, AssetID const& assetID,
        int64_t assetAvailableForTestActivity, int64_t spareLimitAfterSetup,
        uint32_t allowTrustFlags)
    {
        return TrustLineParameters(trustor, assetID,
                                   assetAvailableForTestActivity,
                                   spareLimitAfterSetup, true, allowTrustFlags);
    }

    static TrustLineParameters constexpr withSponsor(
        int trustor, AssetID const& assetID,
        int64_t assetAvailableForTestActivity, int64_t spareLimitAfterSetup,
        int sponsorKey)
    {
        return TrustLineParameters(trustor, assetID,
                                   assetAvailableForTestActivity,
                                   spareLimitAfterSetup, false, 0, sponsorKey);
    }

    static TrustLineParameters constexpr withAllowTrustAndSponsor(
        int trustor, AssetID const& assetID,
        int64_t assetAvailableForTestActivity, int64_t spareLimitAfterSetup,
        uint32_t allowTrustFlags, int sponsorKey)
    {
        return TrustLineParameters(
            trustor, assetID, assetAvailableForTestActivity,
            spareLimitAfterSetup, true, allowTrustFlags, sponsorKey);
    }

    int const mTrustor;
    AssetID const mAssetID;
    int64_t const mAssetAvailableForTestActivity;
    int64_t const mSpareLimitAfterSetup;
    bool const mCallAllowTrustOp;
    uint32_t const mAllowTrustFlags;

  private:
    constexpr TrustLineParameters(int const trustor, AssetID const& assetID,
                                  int64_t assetAvailableForTestActivity,
                                  int64_t spareLimitAfterSetup,
                                  bool callAllowTrustOp,
                                  uint32_t allowTrustFlags)
        : SponsoredEntryParameters()
        , mTrustor(trustor)
        , mAssetID(assetID)
        , mAssetAvailableForTestActivity(assetAvailableForTestActivity)
        , mSpareLimitAfterSetup(spareLimitAfterSetup)
        , mCallAllowTrustOp(callAllowTrustOp)
        , mAllowTrustFlags(allowTrustFlags)
    {
    }

    constexpr TrustLineParameters(int const trustor, AssetID const& assetID,
                                  int64_t assetAvailableForTestActivity,
                                  int64_t spareLimitAfterSetup,
                                  bool callAllowTrustOp,
                                  uint32_t allowTrustFlags, int sponsorKey)
        : SponsoredEntryParameters(sponsorKey)
        , mTrustor(trustor)
        , mAssetID(assetID)
        , mAssetAvailableForTestActivity(assetAvailableForTestActivity)
        , mSpareLimitAfterSetup(spareLimitAfterSetup)
        , mCallAllowTrustOp(callAllowTrustOp)
        , mAllowTrustFlags(allowTrustFlags)
    {
    }
};

std::array<TrustLineParameters, 12> constexpr trustLineParameters{
    {// this trustline will be used to increase native buying liabilites
     TrustLineParameters::withSponsor(0, AssetID(4), INT64_MAX, 0, 2),

     // these trustlines are required for offers
     {2, AssetID(1), 256, 256},
     {3, AssetID(1), 256, 0}, // No available limit left
     {4, AssetID(1), 256, 256},

     {1, AssetID(2), 256, 256},
     {3, AssetID(2), 256, 256},
     {4, AssetID(2), 256, 0}, // No available limit left

     // these 5 trustlines are required for claimable balances
     {2, AssetID(4), 256, 256},
     {3, AssetID(4), INT64_MAX, 0},
     TrustLineParameters::withAllowTrust(4, AssetID(3), INT64_MAX, 0,
                                         AUTHORIZED_FLAG),

     // deauthorize trustline
     TrustLineParameters::withAllowTrustAndSponsor(0, AssetID(1), 0, 256, 0, 1),

     TrustLineParameters::withAllowTrustAndSponsor(
         0, AssetID(2), 0, 256, AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG, 1)

    }};

struct ClaimableBalanceParameters : public SponsoredEntryParameters
{
    constexpr ClaimableBalanceParameters(int const sender, int const claimant,
                                         AssetID const& asset, int64_t amount)
        : SponsoredEntryParameters()
        , mSender(sender)
        , mClaimant(claimant)
        , mAsset(asset)
        , mAmount(amount)
    {
    }

    constexpr ClaimableBalanceParameters(int const sender, int const claimant,
                                         AssetID const& asset, int64_t amount,
                                         int sponsorKey)
        : SponsoredEntryParameters(sponsorKey)
        , mSender(sender)
        , mClaimant(claimant)
        , mAsset(asset)
        , mAmount(amount)
    {
    }

    int const mSender;
    int const mClaimant;
    AssetID const mAsset;
    int64_t const mAmount;
};

std::array<ClaimableBalanceParameters, 11> constexpr claimableBalanceParameters{
    {{1, 2, AssetID(), 10},     // native asset
     {2, 3, AssetID(4), 5},     // non-native asset
     {4, 2, AssetID(4), 20, 2}, // sponsored by account 2
     {4, 3, AssetID(3), 30},    // issuer is claimant
     {1, 3, AssetID(1), 100},   // 3 has no available limit
     {1, 0, AssetID(2),
      1}, // claimant trustline is AUTHORIZED_TO_MAINTAIN_LIABILITIES
     {2, 0, AssetID(), 100000}, // 0 does not have enough native limit

     // leave 0 with a small native balance so it can create a native buy
     // offer for INT64_MAX - balance
     {0, 1, AssetID(),
      INITIAL_ACCOUNT_BALANCE -
          (MIN_ACCOUNT_BALANCE + (2 * FuzzUtils::FUZZING_RESERVE) + 1),
      2},

     {3, 0, AssetID(3), 30}, // 0 has no trustline to this asset
     {3, 0, AssetID(1), 30}, // claimant trustline is not authorized
     // enough limit to claim. trustline is clawback enabled
     {1, 2, AssetID(1), 100}}};

struct OfferParameters : public SponsoredEntryParameters
{
    constexpr OfferParameters(int publicKey, AssetID const& bid,
                              AssetID const& sell, int64_t amount,
                              int32_t priceNumerator, int32_t priceDenominator,
                              bool passive)
        : SponsoredEntryParameters()
        , mPublicKey(publicKey)
        , mBid(bid)
        , mSell(sell)
        , mAmount(amount)
        , mNumerator(priceNumerator)
        , mDenominator(priceDenominator)
        , mPassive(passive)
    {
    }

    constexpr OfferParameters(int publicKey, AssetID const& bid,
                              AssetID const& sell, int64_t amount,
                              int32_t priceNumerator, int32_t priceDenominator,
                              bool passive, int sponsorKey)
        : SponsoredEntryParameters(sponsorKey)
        , mPublicKey(publicKey)
        , mBid(bid)
        , mSell(sell)
        , mAmount(amount)
        , mNumerator(priceNumerator)
        , mDenominator(priceDenominator)
        , mPassive(passive)
    {
    }

    int const mPublicKey;
    AssetID const mBid;
    AssetID const mSell;
    int64_t const mAmount;
    int32_t const mNumerator;
    int32_t const mDenominator;
    bool const mPassive;
};

std::array<OfferParameters, 17> constexpr orderBookParameters{{

    // The first two order books follow this structure
    // +------------+-----+------+--------+------------------------------+
    // |  Account   | Bid | Sell | Amount | Price (in terms of Sell/Bid) |
    // +------------+-----+------+--------+------------------------------+
    // | non-issuer | A   | B    |     10 | 3/2                          |
    // | issuer     | A   | B    |     50 | 3/2                          |
    // | non-issuer | A   | B    |    100 | 1/1 (passive)                |
    // | non-issuer | B   | A    |    100 | 1/1 (passive)                |
    // | issuer     | B   | A    |     10 | 10/9                         |
    // | non-issuer | B   | A    |     50 | 10/9                         |
    // | non-issuer | B   | A    |    100 | 22/7                         |
    // +------------+-----+------+--------+------------------------------+

    // This is a simple order book between a native and non-native asset
    {2, AssetID(), AssetID(1), 10, 3, 2, false},
    {1, AssetID(), AssetID(1), 50, 3, 2, false, 3}, // sponsored by account 3
    {3, AssetID(), AssetID(1), 100, 1, 1, true},
    {3, AssetID(1), AssetID(), 100, 1, 1, true},
    {1, AssetID(1), AssetID(), 10, 10, 9, false},
    {2, AssetID(1), AssetID(), 50, 10, 9, false},
    {2, AssetID(1), AssetID(), 100, 22, 7, false},

    // This is a simple order book between two non-native assets
    {3, AssetID(1), AssetID(2), 10, 3, 2, false},
    {1, AssetID(1), AssetID(2), 50, 3, 2, false, 3}, // sponsored by account 3
    {3, AssetID(1), AssetID(2), 100, 1, 1, true},
    {3, AssetID(2), AssetID(1), 100, 1, 1, true},
    {1, AssetID(2), AssetID(1), 10, 10, 9, false},
    {3, AssetID(2), AssetID(1), 50, 10, 9, false},
    {3, AssetID(2), AssetID(1), 100, 22, 7, false},

    {4, AssetID(4), AssetID(3), INT64_MAX - 50, 1, 1, false},

    // offer to trade all of one asset to another up to the trustline limit
    {4, AssetID(2), AssetID(), 256, 1, 1, true},

    // Increase native buying liabilites for account 0
    {0, AssetID(), AssetID(4),
     INT64_MAX - (MIN_ACCOUNT_BALANCE + (2 * FuzzUtils::FUZZING_RESERVE) + 1),
     1, 1, false, 2}}};

struct PoolSetupParameters : public SponsoredEntryParameters
{
    constexpr PoolSetupParameters(int trustor, AssetID const& assetA,
                                  AssetID const& assetB, int64_t maxAmountA,
                                  int64_t maxAmountB, int32_t minPriceNumerator,
                                  int32_t minPriceDenominator,
                                  int32_t maxPriceNumerator,
                                  int32_t maxPriceDenominator, int64_t limit)
        : SponsoredEntryParameters()
        , mTrustor(trustor)
        , mAssetA(assetA)
        , mAssetB(assetB)
        , mMaxAmountA(maxAmountA)
        , mMaxAmountB(maxAmountB)
        , mMinPriceNumerator(minPriceNumerator)
        , mMinPriceDenominator(minPriceDenominator)
        , mMaxPriceNumerator(maxPriceNumerator)
        , mMaxPriceDenominator(maxPriceDenominator)
        , mLimit(limit)
    {
    }

    constexpr PoolSetupParameters(int trustor, AssetID const& assetA,
                                  AssetID const& assetB, int64_t maxAmountA,
                                  int64_t maxAmountB, int32_t minPriceNumerator,
                                  int32_t minPriceDenominator,
                                  int32_t maxPriceNumerator,
                                  int32_t maxPriceDenominator, int64_t limit,
                                  int sponsorKey)
        : SponsoredEntryParameters(sponsorKey)
        , mTrustor(trustor)
        , mAssetA(assetA)
        , mAssetB(assetB)
        , mMaxAmountA(maxAmountA)
        , mMaxAmountB(maxAmountB)
        , mMinPriceNumerator(minPriceNumerator)
        , mMinPriceDenominator(minPriceDenominator)
        , mMaxPriceNumerator(maxPriceNumerator)
        , mMaxPriceDenominator(maxPriceDenominator)
        , mLimit(limit)
    {
    }

    int const mTrustor;
    AssetID const mAssetA;
    AssetID const mAssetB;
    int64_t const mMaxAmountA;
    int64_t const mMaxAmountB;
    int32_t const mMinPriceNumerator;
    int32_t const mMinPriceDenominator;
    int32_t const mMaxPriceNumerator;
    int32_t const mMaxPriceDenominator;
    int64_t const mLimit;
};

// NUM_STORED_POOL_IDS - 1 because we will push in a hash for a pool that
// doesn't exist into mStoredPoolIDs later
std::array<PoolSetupParameters,
           FuzzUtils::NUM_STORED_POOL_IDS - 1> constexpr poolSetupParameters{
    {// Native 1:1
     {1, AssetID(), AssetID(1), 1000, 1000, 1, 1, 1, 1, 1000},
     // Non-native 2:1
     {2, AssetID(1), AssetID(2), 1000, 500, 2, 1, 2, 1, 1000},
     // Non-native 1:2 sponsored by account 4
     {3, AssetID(1), AssetID(3), 500, 1000, 1, 2, 1, 2, 1000, 4},
     // Native no deposit
     {3, AssetID(), AssetID(4), 0, 0, 0, 0, 0, 0, 1000},
     // Non-native no deposit
     {3, AssetID(2), AssetID(4), 0, 0, 0, 0, 0, 0, 1000},
     // close to max reserves
     {3, AssetID(3), AssetID(4), INT64_MAX - 50, INT64_MAX - 50, 1, 1, 1, 1,
      INT64_MAX}}};

} // anonymous namespace

std::string
TxFuzzTarget::name() const
{
    return "tx";
}

std::string
TxFuzzTarget::description() const
{
    return "Fuzz transaction application with compact XDR operations";
}

void
TxFuzzTarget::initialize()
{
    reinitializeAllGlobalStateForFuzzing(1);
    mApp = createTestApplication(mClock, getFuzzConfig(0));
    OrderBookIsNotCrossed::registerAndEnableInvariant(*mApp);
    auto root = mApp->getRoot();
    mSourceAccountID = root->getPublicKey();

    resetTxInternalState(*mApp);
    LedgerTxn ltxOuter(mApp->getLedgerTxnRoot());

    initializeAccounts(ltxOuter);
    initializeTrustLines(ltxOuter);
    initializeClaimableBalances(ltxOuter);
    initializeOffers(ltxOuter);
    initializeLiquidityPools(ltxOuter);
    reduceNativeBalancesAfterSetup(ltxOuter);
    adjustTrustLineBalancesAfterSetup(ltxOuter);
    reduceTrustLineLimitsAfterSetup(ltxOuter);
    storeSetupLedgerKeysAndPoolIDs(ltxOuter);

    ltxOuter.commit();

#ifdef BUILD_TESTS
    mApp->getInvariantManager().snapshotForFuzzer();
#endif
}

FuzzResultCode
TxFuzzTarget::run(uint8_t const* data, size_t size)
{
    if (data == nullptr || size == 0 || size > maxInputSize())
    {
        return FuzzResultCode::FUZZ_REJECTED;
    }

    xdr::xvector<Operation> ops;
    std::vector<char> bins(data, data + size);

    try
    {
        xdr::xdr_from_fuzzer_opaque(mStoredLedgerKeys, mStoredPoolIDs, bins,
                                    ops);
    }
    catch (std::exception const& e)
    {
        LOG_TRACE(DEFAULT_LOG,
                  "xdr::xdr_from_fuzzer_opaque() threw exception {}", e.what());
        return FuzzResultCode::FUZZ_REJECTED;
    }

    if (ops.size() < 1 || ops.size() > FUZZER_MAX_OPERATIONS)
    {
        LOG_TRACE(DEFAULT_LOG, "invalid ops.size() {}", ops.size());
        return FuzzResultCode::FUZZ_REJECTED;
    }

    resetTxInternalState(*mApp);
    LOG_TRACE(DEFAULT_LOG, "{}",
              xdrToCerealString(ops, fmt::format("Fuzz ops ({})", ops.size())));

    LedgerTxn ltx(mApp->getLedgerTxnRoot());
    applyFuzzOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);

    return FuzzResultCode::FUZZ_SUCCESS;
}

void
TxFuzzTarget::shutdown()
{
    mApp.reset();
}

size_t
TxFuzzTarget::maxInputSize() const
{
    // 50 bytes in compact mode seems to hold large operations
    return 50 * FUZZER_MAX_OPERATIONS;
}

std::vector<uint8_t>
TxFuzzTarget::generateSeedInput()
{
    autocheck::generator<Operation> gen;
    xdr::xvector<Operation> ops;
    ops.reserve(FUZZER_MAX_OPERATIONS);
    auto const numops = rand_uniform<int>(1, FUZZER_MAX_OPERATIONS);
    for (int j = 0; j < numops; ++j)
    {
        Operation op = gen(128);
        if (op.body.type() == INVOKE_HOST_FUNCTION ||
            op.body.type() == EXTEND_FOOTPRINT_TTL ||
            op.body.type() == RESTORE_FOOTPRINT)
        {
            continue;
        }
        if (!op.sourceAccount)
        {
            PublicKey a0;
            FuzzUtils::setShortKey(a0, 0);
            op.sourceAccount.activate() = toMuxedAccount(a0);
        }
        ops.emplace_back(op);
    }
    auto bins = xdr::xdr_to_fuzzer_opaque(ops);
    return std::vector<uint8_t>(bins.begin(), bins.end());
}

void
TxFuzzTarget::storeSetupPoolIDs(AbstractLedgerTxn& ltx,
                                std::vector<LedgerEntry> const& entries)
{
    std::vector<PoolID> poolIDs;
    for (auto const& entry : entries)
    {
        if (entry.data.type() != LIQUIDITY_POOL)
        {
            continue;
        }
        poolIDs.emplace_back(entry.data.liquidityPool().liquidityPoolID);
    }

    assert(poolIDs.size() == FuzzUtils::NUM_STORED_POOL_IDS - 1);
    auto firstGeneratedPoolID =
        std::copy(poolIDs.cbegin(), poolIDs.cend(), mStoredPoolIDs.begin());
    std::generate(firstGeneratedPoolID, mStoredPoolIDs.end(),
                  []() { return PoolID{}; });
}

void
TxFuzzTarget::storeSetupLedgerKeysAndPoolIDs(AbstractLedgerTxn& ltx)
{
    std::vector<LedgerEntry> init, live;
    std::vector<LedgerKey> dead;
    ltx.getAllEntries(init, live, dead);

    std::sort(init.begin(), init.end());

    assert(dead.empty());
    if (live.size() == 1)
    {
        assert(live[0].data.type() == ACCOUNT);
        assert(live[0].data.account().accountID ==
               txtest::getRoot(mApp->getNetworkID()).getPublicKey());
    }
    else
    {
        assert(live.empty());
    }

    assert(init.size() <= FuzzUtils::NUM_VALIDATED_LEDGER_KEYS);

    auto firstGeneratedLedgerKey = std::transform(
        init.cbegin(), init.cend(), mStoredLedgerKeys.begin(), LedgerEntryKey);

    FuzzUtils::generateStoredLedgerKeys(firstGeneratedLedgerKey,
                                        mStoredLedgerKeys.end());

    storeSetupPoolIDs(ltx, init);
}

void
TxFuzzTarget::initializeAccounts(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);
    xdr::xvector<Operation> ops;

    for (auto const& param : accountParameters)
    {
        PublicKey publicKey;
        FuzzUtils::setShortKey(publicKey, param.mShortKey);

        emplaceConditionallySponsored(
            ops, txtest::createAccount(publicKey, INITIAL_ACCOUNT_BALANCE),
            param.mSponsored, param.mSponsorKey, publicKey);

        // Set options for any accounts whose parameters specify flags to
        // add.
        auto const optionFlags = param.mOptionFlags;
        if (optionFlags != 0)
        {
            auto optionsOp = txtest::setOptions(txtest::setFlags(optionFlags));
            optionsOp.sourceAccount.activate() = toMuxedAccount(publicKey);
            ops.emplace_back(optionsOp);
        }
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);
    ltx.commit();
}

void
TxFuzzTarget::initializeTrustLines(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);
    xdr::xvector<Operation> ops;

    for (auto const& trustLine : trustLineParameters)
    {
        auto const trustor = trustLine.mTrustor;
        PublicKey account;
        FuzzUtils::setShortKey(account, trustor);

        auto const asset = trustLine.mAssetID.toAsset();

        // Trust the asset issuer.
        auto trustOp = txtest::changeTrust(
            asset, std::max<int64_t>(INITIAL_TRUST_LINE_LIMIT,
                                     trustLine.mAssetAvailableForTestActivity));
        trustOp.sourceAccount.activate() = toMuxedAccount(account);
        emplaceConditionallySponsored(ops, trustOp, trustLine.mSponsored,
                                      trustLine.mSponsorKey, account);

        PublicKey issuer;
        auto const issuerID = trustLine.mAssetID.mIssuer;
        FuzzUtils::setShortKey(issuer, issuerID);

        // Set trust line flags if specified.
        if (trustLine.mCallAllowTrustOp)
        {
            auto allowTrustOp =
                txtest::allowTrust(account, asset, trustLine.mAllowTrustFlags);
            allowTrustOp.sourceAccount.activate() = toMuxedAccount(issuer);
            ops.emplace_back(allowTrustOp);
        }

        if (!trustLine.mCallAllowTrustOp ||
            trustLine.mAllowTrustFlags & AUTHORIZED_FLAG)
        {
            // Distribute the starting amount of the asset (to be reduced after
            // orders have been placed).
            auto distributeOp = txtest::payment(
                account, asset,
                std::max<int64_t>(INITIAL_ASSET_DISTRIBUTION,
                                  trustLine.mAssetAvailableForTestActivity));
            distributeOp.sourceAccount.activate() = toMuxedAccount(issuer);
            ops.emplace_back(distributeOp);
        }
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);
    ltx.commit();
}

void
TxFuzzTarget::initializeClaimableBalances(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);
    xdr::xvector<Operation> ops;

    for (auto const& param : claimableBalanceParameters)
    {
        Claimant claimant;
        claimant.v0().predicate.type(CLAIM_PREDICATE_UNCONDITIONAL);
        FuzzUtils::setShortKey(claimant.v0().destination, param.mClaimant);

        auto claimableBalanceOp = txtest::createClaimableBalance(
            param.mAsset.toAsset(), param.mAmount, {claimant});

        PublicKey senderKey;
        FuzzUtils::setShortKey(senderKey, param.mSender);

        claimableBalanceOp.sourceAccount.activate() = toMuxedAccount(senderKey);
        emplaceConditionallySponsored(ops, claimableBalanceOp, param.mSponsored,
                                      param.mSponsorKey, senderKey);
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);
    ltx.commit();
}

void
TxFuzzTarget::initializeOffers(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);
    xdr::xvector<Operation> ops;

    for (auto const& param : orderBookParameters)
    {
        auto op = param.mPassive
                      ? txtest::createPassiveOffer(
                            param.mSell.toAsset(), param.mBid.toAsset(),
                            Price{param.mNumerator, param.mDenominator},
                            param.mAmount)
                      : txtest::manageOffer(
                            0, param.mSell.toAsset(), param.mBid.toAsset(),
                            Price{param.mNumerator, param.mDenominator},
                            param.mAmount);
        PublicKey pkA;
        FuzzUtils::setShortKey(pkA, param.mPublicKey);
        op.sourceAccount.activate() = toMuxedAccount(pkA);
        emplaceConditionallySponsored(ops, op, param.mSponsored,
                                      param.mSponsorKey, pkA);
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);
    ltx.commit();
}

void
TxFuzzTarget::initializeLiquidityPools(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);
    xdr::xvector<Operation> ops;

    for (auto const& param : poolSetupParameters)
    {
        auto const trustor = param.mTrustor;
        PublicKey account;
        FuzzUtils::setShortKey(account, trustor);

        auto const assetA = param.mAssetA.toAsset();
        auto const assetB = param.mAssetB.toAsset();

        ChangeTrustAsset poolAsset;
        poolAsset.type(ASSET_TYPE_POOL_SHARE);
        poolAsset.liquidityPool().constantProduct().assetA = assetA;
        poolAsset.liquidityPool().constantProduct().assetB = assetB;
        poolAsset.liquidityPool().constantProduct().fee =
            LIQUIDITY_POOL_FEE_V18;

        auto trustOp = txtest::changeTrust(
            poolAsset,
            std::max<int64_t>(INITIAL_TRUST_LINE_LIMIT, param.mLimit));
        trustOp.sourceAccount.activate() = toMuxedAccount(account);
        emplaceConditionallySponsored(ops, trustOp, param.mSponsored,
                                      param.mSponsorKey, account);

        if (param.mMaxAmountA > 0 && param.mMaxAmountB > 0)
        {
            auto depositOp = txtest::liquidityPoolDeposit(
                xdrSha256(poolAsset.liquidityPool()), param.mMaxAmountA,
                param.mMaxAmountB,
                Price{param.mMinPriceNumerator, param.mMinPriceDenominator},
                Price{param.mMaxPriceNumerator, param.mMaxPriceDenominator});
            depositOp.sourceAccount.activate() = toMuxedAccount(account);
            emplaceConditionallySponsored(ops, depositOp, param.mSponsored,
                                          param.mSponsorKey, account);
        }
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);
    ltx.commit();
}

void
TxFuzzTarget::reduceNativeBalancesAfterSetup(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);
    xdr::xvector<Operation> ops;

    for (auto const& param : accountParameters)
    {
        PublicKey account;
        FuzzUtils::setShortKey(account, param.mShortKey);

        // Reduce "account"'s native balance by paying the *root, so that
        // fuzzing has a better chance of exercising edge cases.
        auto ae = stellar::loadAccount(ltx, account);
        auto const availableBalance = getAvailableBalance(ltx.loadHeader(), ae);
        auto const targetAvailableBalance =
            param.mNativeAssetAvailableForTestActivity + MIN_ACCOUNT_BALANCE;

        assert(availableBalance > targetAvailableBalance);
        auto reduceNativeBalanceOp = txtest::payment(
            mSourceAccountID, availableBalance - targetAvailableBalance);
        reduceNativeBalanceOp.sourceAccount.activate() =
            toMuxedAccount(account);
        ops.emplace_back(reduceNativeBalanceOp);
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);
    ltx.commit();
}

void
TxFuzzTarget::adjustTrustLineBalancesAfterSetup(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);
    xdr::xvector<Operation> ops;

    // Reduce trustline balances so that fuzzing has a better chance of
    // exercising edge cases.
    for (auto const& trustLine : trustLineParameters)
    {
        auto const trustor = trustLine.mTrustor;
        PublicKey account;
        FuzzUtils::setShortKey(account, trustor);

        auto const asset = trustLine.mAssetID.toAsset();

        PublicKey issuer;
        FuzzUtils::setShortKey(issuer, trustLine.mAssetID.mIssuer);

        // Reduce "account"'s balance of this asset by paying the
        // issuer.
        auto tle = stellar::loadTrustLine(ltx, account, asset);
        if (!tle.isAuthorizedToMaintainLiabilities())
        {
            // Without authorization, this trustline could not have been funded
            // with how the setup currently works
            if (trustLine.mAssetAvailableForTestActivity != 0 ||
                tle.getBalance() != 0)
            {
                throw std::runtime_error("Invalid trustline setup");
            }
            continue;
        }

        auto const maxRecv = tle.getMaxAmountReceive(ltx.loadHeader());
        auto const availableTLBalance =
            tle.getAvailableBalance(ltx.loadHeader());
        auto const targetAvailableTLBalance =
            trustLine.mAssetAvailableForTestActivity;
        auto const paymentAmount =
            availableTLBalance - targetAvailableTLBalance;

        if (availableTLBalance > targetAvailableTLBalance)
        {
            auto reduceNonNativeBalanceOp =
                txtest::payment(issuer, asset, paymentAmount);
            reduceNonNativeBalanceOp.sourceAccount.activate() =
                toMuxedAccount(account);
            ops.emplace_back(reduceNonNativeBalanceOp);
        }
        else if (availableTLBalance < targetAvailableTLBalance && maxRecv > 0 &&
                 (!trustLine.mCallAllowTrustOp ||
                  trustLine.mAllowTrustFlags & AUTHORIZED_FLAG))
        {
            auto increaseNonNativeBalanceOp = txtest::payment(
                account, asset,
                std::min(targetAvailableTLBalance - availableTLBalance,
                         maxRecv));
            increaseNonNativeBalanceOp.sourceAccount.activate() =
                toMuxedAccount(issuer);
            ops.emplace_back(increaseNonNativeBalanceOp);
        }
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);
    ltx.commit();
}

void
TxFuzzTarget::reduceTrustLineLimitsAfterSetup(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);
    xdr::xvector<Operation> ops;

    // Reduce trustline limits so that fuzzing has a better chance of exercising
    // edge cases.
    for (auto const& trustLine : trustLineParameters)
    {
        auto const trustor = trustLine.mTrustor;
        PublicKey account;
        FuzzUtils::setShortKey(account, trustor);

        auto const asset = trustLine.mAssetID.toAsset();

        // Reduce this trustline's limit.
        auto tle = stellar::loadTrustLine(ltx, account, asset);
        auto const balancePlusBuyLiabilities =
            tle.getBalance() + tle.getBuyingLiabilities(ltx.loadHeader());
        auto const targetTrustLineLimit =
            INT64_MAX - trustLine.mSpareLimitAfterSetup <
                    balancePlusBuyLiabilities
                ? INT64_MAX
                : balancePlusBuyLiabilities + trustLine.mSpareLimitAfterSetup;

        auto changeTrustLineLimitOp =
            txtest::changeTrust(asset, targetTrustLineLimit);
        changeTrustLineLimitOp.sourceAccount.activate() =
            toMuxedAccount(account);
        ops.emplace_back(changeTrustLineLimitOp);
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);
    ltx.commit();
}

// Register the target with the global registry
REGISTER_FUZZ_TARGET(
    TxFuzzTarget, "tx",
    "Fuzz transaction application with compact XDR operations");

} // namespace stellar
