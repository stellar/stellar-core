// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/FuzzerImpl.h"
#include "ledger/LedgerTxn.h"
#include "ledger/TrustLineWrapper.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "overlay/TCPPeer.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/fuzz.h"
#include "test/test.h"
#include "transactions/OperationFrame.h"
#include "transactions/SignatureChecker.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/XDRCereal.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"

#include <exception>
#include <fmt/format.h>
#include <xdrpp/autocheck.h>

namespace stellar
{
namespace FuzzUtils
{
auto constexpr FUZZER_MAX_OPERATIONS = 5;
auto constexpr INITIAL_ACCOUNT_BALANCE = 1'000'000LL;    // reduced after setup
auto constexpr INITIAL_ASSET_DISTRIBUTION = 1'000'000LL; // reduced after setup
auto constexpr NUMBER_OF_ASSETS_TO_ISSUE = 4U;
auto constexpr FUZZING_FEE = 1;
auto constexpr FUZZING_RESERVE = 4;
auto constexpr INITIAL_TRUST_LINE_LIMIT = 5 * INITIAL_ASSET_DISTRIBUTION;
auto constexpr DEFAULT_NUM_TRANSACTIONS_TO_RESERVE_FEES_FOR = 10;
auto constexpr DEFAULT_ASSET_AVAILABLE_FOR_TEST_ACTIVITY = 256;

// must be strictly less than 255
uint8_t constexpr NUMBER_OF_PREGENERATED_ACCOUNTS = 16U;

void
setShortKey(uint256& ed25519, int i)
{
    ed25519[0] = static_cast<uint8_t>(i);
}

void
setShortKey(PublicKey& pk, int i)
{
    setShortKey(pk.ed25519(), i);
}

uint8_t
getShortKey(uint256 const& ed25519)
{
    return ed25519[0];
}

uint8_t
getShortKey(PublicKey const& pk)
{
    return getShortKey(pk.ed25519());
}

// constructs an Asset structure for an asset issued by an account id comprised
// of bytes reading [0,0,...,i] and an alphanum4 asset code of "Ast + i"
Asset
makeAsset(int i)
{
    Asset asset;
    asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(asset.alphaNum4().assetCode, "Ast" + std::to_string(i));
    setShortKey(asset.alphaNum4().issuer, i);
    return asset;
}
}
}

namespace xdr
{
/*
    the xdr_fuzzer_compactor/xdr_fuzzer_unpacker helper structs
    are based on xdr_get/xdr_put (marshallers for xdr) and make the following
   adjustments:
    * use a binary representation as compact as possible, so that fuzzers have
   less data to fuzz
        * shorten 64 and 32 bits values into respectively 16 and 8 bits
            * in particular, discriminant values are 8 bits instead of 32
        * shorten byte arrays
            * static arrays of size N bytes are shorten to 1 byte
            * non empty variable size arrays are shortened to 1 byte
        * remaps complex types
            * PublicKey is mapped to 8 bits
    * use the lowest overhead possible binary form
        * no alignment requirement
        * does not adjust endianness
        * implementation defined behavior (generation and fuzzing must be
               from the same build, on the same arch)
*/
struct xdr_fuzzer_compactor
{
    std::uint8_t* const mStart;
    std::uint8_t* mCur;
    std::uint8_t* const mEnd;

    xdr_fuzzer_compactor(void* start, void* end)
        : mStart(reinterpret_cast<std::uint8_t*>(start))
        , mCur(reinterpret_cast<std::uint8_t*>(start))
        , mEnd(reinterpret_cast<std::uint8_t*>(end))
    {
        assert(mStart <= mEnd);
    }
    xdr_fuzzer_compactor(msg_ptr& m) : xdr_fuzzer_compactor(m->data(), m->end())
    {
    }

    void
    put_bytes(void const* buf, size_t len)
    {
        if (len != 0)
        {
            std::memcpy(mCur, buf, len);
            mCur += len;
        }
    }

    void
    check(std::size_t n) const
    {
        if (n > std::size_t(reinterpret_cast<char*>(mEnd) -
                            reinterpret_cast<char*>(mCur)))
            throw xdr_overflow(
                "insufficient buffer space in xdr_fuzzer_compactor");
    }

    uint32_t
    size() const
    {
        auto s = std::size_t(reinterpret_cast<char*>(mCur) -
                             reinterpret_cast<char*>(mStart));
        return static_cast<uint32_t>(s);
    }

    template <typename T>
    typename std::enable_if<std::is_same<
        std::uint32_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T t)
    {
        // convert uint32 -> 1 byte
        check(1);
        auto v = xdr_traits<T>::to_uint(t);
        uint8_t b = static_cast<uint8_t>(v & 0xFF);
        put_bytes(&b, 1);
    }

    template <typename T>
    typename std::enable_if<std::is_same<
        std::uint64_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T t)
    {
        // convert uint64 -> 2 bytes
        check(2);
        uint16_t v = static_cast<uint16_t>(xdr_traits<T>::to_uint(t) & 0xFFFF);
        put_bytes(&v, 2);
    }

    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_bytes>::type
    operator()(T const& t)
    {
        // convert array -> 0/1 byte
        uint8_t s2 = t.empty() ? 0 : 1;
        if (xdr_traits<T>::variable_nelem)
        {
            check(1 + s2);
            put_bytes(&s2, 1);
        }
        else
        {
            check(s2);
        }
        put_bytes(t.data(), s2);
    }

    template <typename T>
    typename std::enable_if<(!std::is_same<stellar::AccountID, T>::value &&
                             !std::is_same<stellar::MuxedAccount, T>::value) &&
                            (xdr_traits<T>::is_class ||
                             xdr_traits<T>::is_container)>::type
    operator()(T const& t)
    {
        xdr_traits<T>::save(*this, t);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::AccountID, T>::value>::type
    operator()(T const& pk)
    {
        // convert public key 1 byte
        check(1);
        auto b = stellar::FuzzUtils::getShortKey(pk.ed25519());
        put_bytes(&b, 1);
    }
    template <typename T>
    typename std::enable_if<std::is_same<stellar::MuxedAccount, T>::value>::type
    operator()(T const& m)
    {
        // convert MuxedAccount -> 1 byte (same than an AccountID)
        auto const& ed25519 = (m.type() == stellar::KEY_TYPE_ED25519)
                                  ? m.ed25519()
                                  : m.med25519().ed25519;
        check(1);
        auto b = stellar::FuzzUtils::getShortKey(ed25519);
        put_bytes(&b, 1);
    }
};

template <typename... Args>
opaque_vec<>
xdr_to_fuzzer_opaque(Args const&... args)
{
    opaque_vec<> m(opaque_vec<>::size_type{xdr_argpack_size(args...)});
    xdr_fuzzer_compactor p(m.data(), m.data() + m.size());
    xdr_argpack_archive(p, args...);
    m.resize(p.size());
    return m;
}

struct xdr_fuzzer_unpacker
{
    std::uint8_t const* mCur;
    std::uint8_t const* const mEnd;

    xdr_fuzzer_unpacker(void const* start, void const* end)
        : mCur(reinterpret_cast<std::uint8_t const*>(start))
        , mEnd(reinterpret_cast<std::uint8_t const*>(end))
    {
        assert(mCur <= mEnd);
    }
    xdr_fuzzer_unpacker(msg_ptr const& m)
        : xdr_fuzzer_unpacker(m->data(), m->end())
    {
    }

    void
    get_bytes(void* buf, size_t len)
    {
        if (len != 0)
        {
            std::memcpy(buf, mCur, len);
            mCur += len;
        }
    }

    uint8_t
    get_byte()
    {
        uint8_t b;
        get_bytes(&b, 1);
        return b;
    }

    void
    check(std::size_t n) const
    {
        if (n > std::size_t(reinterpret_cast<char const*>(mCur) -
                            reinterpret_cast<char const*>(mEnd)))
            throw xdr_overflow(
                "insufficient buffer space in xdr_fuzzer_unpacker");
    }

    template <typename T>
    typename std::enable_if<std::is_same<
        std::uint32_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T& t)
    {
        // 1 byte --> uint32
        check(1);
        uint32_t w = get_byte();
        t = xdr_traits<T>::from_uint(w);
    }

    template <typename T>
    typename std::enable_if<std::is_same<
        std::uint64_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T& t)
    {
        // 2 bytes --> uint64 **with** "sign extension"
        check(2);
        // load into a 16 signed
        int16_t w;
        get_bytes(&w, 2);
        // extend to 64 bit
        int64_t ww = w;
        t = xdr_traits<T>::from_uint(ww);
    }

    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_bytes>::type
    operator()(T& t)
    {
        std::uint32_t s2 = 0;
        if (xdr_traits<T>::variable_nelem)
        {
            check(1);
            s2 = get_byte();
            check(s2);
            // only accept small vectors
            if (s2 > 1)
            {
                throw xdr_overflow("large vector in xdr_fuzzer_unpacker");
            }
            t.resize(s2);
        }
        else
        {
            if (!t.empty())
            {
                s2 = 1;
            }
            check(s2);
        }
        get_bytes(t.data(), s2);
    }

    template <typename T>
    typename std::enable_if<(!std::is_same<stellar::AccountID, T>::value &&
                             !std::is_same<stellar::MuxedAccount, T>::value) &&
                            (xdr_traits<T>::is_class ||
                             xdr_traits<T>::is_container)>::type
    operator()(T& t)
    {
        xdr_traits<T>::load(*this, t);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::AccountID, T>::value>::type
    operator()(T& pk)
    {
        // 1 byte --> AccountID
        check(1);
        std::uint8_t v = get_byte();
        stellar::FuzzUtils::setShortKey(pk, v);
    }
    template <typename T>
    typename std::enable_if<std::is_same<stellar::MuxedAccount, T>::value>::type
    operator()(T& m)
    {
        // convert 1 byte --> MuxedAccount (regular AccountID)
        check(1);
        std::uint8_t v = get_byte();
        stellar::FuzzUtils::setShortKey(m.ed25519(), v);
    }

    void
    done()
    {
        if (mCur != mEnd)
        {
            throw xdr_bad_message_size("trailing data in xdr_fuzzer_unpacker");
        }
    }
};

template <typename Bytes, typename... Args>
auto
xdr_from_fuzzer_opaque(Bytes const& m, Args&... args)
    -> decltype(detail::bytes_to_void(m))
{
    xdr_fuzzer_unpacker g(m.data(), m.data() + m.size());
    xdr_argpack_archive(g, args...);
    g.done();
}

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
template <>
void
generator_t::operator()(stellar::PublicKey& t) const
{
    // note that we include NUMBER_OF_PREGENERATED_ACCOUNTS as to also cover the
    // case of non existing accounts
    stellar::FuzzUtils::setShortKey(
        t.ed25519(),
        static_cast<uint8_t>(stellar::rand_uniform<int>(
            0, stellar::FuzzUtils::NUMBER_OF_PREGENERATED_ACCOUNTS)));
}
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
}

namespace stellar
{
// creates a generic configuration with settings rigged to maximize
// determinism
static Config
getFuzzConfig(int instanceNumber)
{
    Config cfg = getTestConfig(instanceNumber);
    cfg.MANUAL_CLOSE = true;
    cfg.CATCHUP_COMPLETE = false;
    cfg.CATCHUP_RECENT = 0;
    cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = false;
    cfg.ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING = UINT32_MAX;
    cfg.HTTP_PORT = 0;
    cfg.WORKER_THREADS = 1;
    cfg.QUORUM_INTERSECTION_CHECKER = false;
    cfg.PREFERRED_PEERS_ONLY = false;
    cfg.RUN_STANDALONE = true;
    cfg.TESTING_UPGRADE_DESIRED_FEE = FuzzUtils::FUZZING_FEE;
    cfg.TESTING_UPGRADE_RESERVE = FuzzUtils::FUZZING_RESERVE;

    return cfg;
}

static void
resetRandomSeed()
{
    // seed randomness
    srand(1);
    gRandomEngine.seed(1);
}

static void
resetTxInternalState(Application& app)
{
    resetRandomSeed();
// reset caches to clear persistent state
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    app.getLedgerTxnRoot().resetForFuzzer();
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    app.getDatabase().clearPreparedStatementCache();
}

// FuzzTransactionFrame is a specialized TransactionFrame that includes
// useful methods for fuzzing such as an attemptApplication method for reseting
// ledger state and deterministically attempting application of transactions.
class FuzzTransactionFrame : public TransactionFrame
{
  public:
    FuzzTransactionFrame(Hash const& networkID,
                         TransactionEnvelope const& envelope)
        : TransactionFrame(networkID, envelope){};

    void
    attemptApplication(Application& app, AbstractLedgerTxn& ltx)
    {
        // reset results of operations
        resetResults(ltx.getHeader(), 0, true);

        // attempt application of transaction without accounting for sequence
        // number, processing the fee, or committing the LedgerTxn
        SignatureChecker signatureChecker{
            ltx.loadHeader().current().ledgerVersion, getContentsHash(),
            mEnvelope.v1().signatures};
        // if any ill-formed Operations, do not attempt transaction application
        auto isInvalidOperationXDR = [&](auto const& op) {
            return !op->checkValid(signatureChecker, ltx, false);
        };
        if (std::any_of(mOperations.begin(), mOperations.end(),
                        isInvalidOperationXDR))
        {
            return;
        }
        // while the following method's result is not captured, regardless, for
        // protocols < 8, this triggered buggy caching, and potentially may do
        // so in the future
        loadSourceAccount(ltx, ltx.loadHeader());
        TransactionMeta tm(2);
        applyOperations(signatureChecker, app, ltx, tm);
        if (getResultCode() == txINTERNAL_ERROR)
        {
            throw std::runtime_error("Internal error while fuzzing");
        }
    }
};

std::shared_ptr<FuzzTransactionFrame>
createFuzzTransactionFrame(PublicKey sourceAccountID,
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
    tx1.tx.seqNum = 1;
    std::copy(begin, end, std::back_inserter(tx1.tx.operations));

    std::shared_ptr<FuzzTransactionFrame> res =
        std::make_shared<FuzzTransactionFrame>(networkID, txEnv);
    return res;
}

bool
isBadOverlayFuzzerInput(StellarMessage const& m)
{
    // HELLO, AUTH and ERROR_MSG messages cause the connection between
    // the peers to drop. Since peer connections are only established
    // preceding the persistent loop, a dropped peer is not only
    // inconvenient, it also confuses the fuzzer. Consider a msg A sent
    // before a peer is dropped and after a peer is dropped. The two,
    // even though the same message, will take drastically different
    // execution paths -- the fuzzer's main metric for determinism
    // (stability) and binary coverage.
    return m.type() == AUTH || m.type() == ERROR_MSG || m.type() == HELLO;
}

// Empties "ops" as operations are applied.  If `throwIfTxFails` is `false`,
// ignores failures and might apply further transactions after failures.
static void
attemptToApplyOps(LedgerTxn& ltx, PublicKey const& sourceAccount,
                  xdr::xvector<Operation>::const_iterator begin,
                  xdr::xvector<Operation>::const_iterator end, Application& app,
                  bool const throwIfTxFails = true)
{
    for (auto beginOpsInThisTx = begin; beginOpsInThisTx != end;)
    {
        auto endOpsInThisTx =
            std::distance(beginOpsInThisTx, end) <= MAX_OPS_PER_TX
                ? end
                : begin + MAX_OPS_PER_TX;
        auto txFramePtr =
            createFuzzTransactionFrame(sourceAccount, beginOpsInThisTx,
                                       endOpsInThisTx, app.getNetworkID());
        txFramePtr->attemptApplication(app, ltx);
        beginOpsInThisTx = endOpsInThisTx;

        if (throwIfTxFails)
        {
            if (txFramePtr->getResultCode() != txSUCCESS)
            {
                auto const msg = fmt::format(
                    FMT_STRING("Error {} while applying operations "
                               "while fuzzing: transaction result {}"),
                    txFramePtr->getResultCode(),
                    xdr_to_string(txFramePtr->getResult()));
                LOG_FATAL(DEFAULT_LOG, "{}", msg);
                throw std::runtime_error(msg);
            }

            for (auto const& opFrame : txFramePtr->getOperations())
            {
                auto const& op = opFrame->getOperation();
                auto const& tr = opFrame->getResult().tr();
                auto const opType = op.body.type();

                if ((opType == MANAGE_BUY_OFFER &&
                     tr.manageBuyOfferResult().success().offer.effect() ==
                         MANAGE_OFFER_DELETED) ||
                    (opType == MANAGE_SELL_OFFER &&
                     tr.manageSellOfferResult().success().offer.effect() ==
                         MANAGE_OFFER_DELETED))
                {
                    auto const msg = fmt::format(
                        FMT_STRING("MANAGE_OFFER_DELETED while setting "
                                   "up fuzzing -- operation is {}"),
                        xdr_to_string(op));
                    LOG_FATAL(DEFAULT_LOG, "{}", msg);
                    throw std::runtime_error(msg);
                }
            }
        }
    }
}

// Unlike Asset, this can be a constexpr.
struct AssetID
{
    constexpr AssetID() : mIsNative(true), mId(0)
    {
    }

    constexpr AssetID(int id) : mIsNative(false), mId(id)
    {
    }

    Asset
    toAsset() const
    {
        return mIsNative ? txtest::makeNativeAsset()
                         : FuzzUtils::makeAsset(mId);
    }

    bool const mIsNative;
    int const mId; // meaningful only if !isNative
};

struct OfferParameters
{
    constexpr OfferParameters(int publicKey, AssetID const bid,
                              AssetID const sell, int64_t amount,
                              int32_t priceNumerator, int32_t priceDenominator)
        : mPublicKey(publicKey)
        , mBid(bid)
        , mSell(sell)
        , mAmount(amount)
        , mNumerator(priceNumerator)
        , mDenominator(priceDenominator)
    {
    }

    int const mPublicKey;
    AssetID const mBid;
    AssetID const mSell;
    int64_t const mAmount;
    int32_t const mNumerator;
    int32_t const mDenominator;
};

// The current order book setup generates identical configurations for the
// following asset pairs:
//      XLM - A
//      A   - B
//      B   - C
//      C   - D
//
// For any asset A and asset B, the generic order book setup will be as
// follows:
//
// +------------+-----+------+--------+------------------------------+
// |  Account   | Bid | Sell | Amount | Price (in terms of Sell/Bid) |
// +------------+-----+------+--------+------------------------------+
// | 0          | A   | B    |     10 | 3/2                          |
// | 1 (issuer) | A   | B    |     50 | 3/2                          |
// | 2          | A   | B    |    100 | 1/1                          |
// | 3 (issuer) | B   | A    |     10 | 10/9                         |
// | 4          | B   | A    |     50 | 10/9                         |
// | 0          | B   | A    |    100 | 22/7                         |
// +------------+-----+------+--------+------------------------------+
//
// (This is far more symmetric than it needs to be; we will introduce more
// variety.  In the long run, we plan to fuzz the setup itself.)
std::array<OfferParameters, 24> constexpr orderBookParameters{
    {{13, AssetID(), AssetID(1), 10, 3, 2}, // asset pair 0
     {14, AssetID(), AssetID(1), 50, 3, 2},
     {15, AssetID(), AssetID(1), 100, 1, 1},
     {1, AssetID(1), AssetID(), 10, 10, 9},
     {12, AssetID(1), AssetID(), 50, 10, 9},
     {13, AssetID(1), AssetID(), 100, 22, 7},

     {11, AssetID(1), AssetID(2), 10, 3, 2}, // asset pair 1
     {1, AssetID(1), AssetID(2), 50, 3, 2},
     {12, AssetID(1), AssetID(2), 100, 1, 1},
     {2, AssetID(2), AssetID(1), 10, 10, 9},
     {10, AssetID(2), AssetID(1), 50, 10, 9},
     {11, AssetID(2), AssetID(1), 100, 22, 7},

     {13, AssetID(2), AssetID(3), 10, 3, 2}, // asset pair 2
     {2, AssetID(2), AssetID(3), 50, 3, 2},
     {14, AssetID(2), AssetID(3), 100, 1, 1},
     {3, AssetID(3), AssetID(2), 10, 10, 9},
     {15, AssetID(3), AssetID(2), 50, 10, 9},
     {13, AssetID(3), AssetID(2), 100, 22, 7},

     {6, AssetID(3), AssetID(4), 10, 3, 2}, // asset pair 3
     {3, AssetID(3), AssetID(4), 50, 3, 2},
     {7, AssetID(3), AssetID(4), 100, 1, 1},
     {4, AssetID(4), AssetID(3), 10, 10, 9},
     {8, AssetID(4), AssetID(3), 50, 10, 9},
     {6, AssetID(4), AssetID(3), 100, 22, 7}}};

void
TransactionFuzzer::initialize()
{
    resetRandomSeed();
    mApp = createTestApplication(mClock, getFuzzConfig(0));
    auto root = TestAccount::createRoot(*mApp);
    mSourceAccountID = root.getPublicKey();

    resetTxInternalState(*mApp);
    LedgerTxn ltxOuter(mApp->getLedgerTxnRoot());

    {
        LedgerTxn ltx(ltxOuter);
        // Setup the state, for this we only need to pregenerate some
        // accounts. For now we create NUMBER_OF_PREGENERATED_ACCOUNTS accounts,
        // or enough to fill the first few bits such that we have a pregenerated
        // account for the last few bits of the 32nd byte of a public key, thus
        // account creation is over a deterministic range of public keys
        xdr::xvector<Operation> ops;
        for (uint8_t i = 0; i < FuzzUtils::NUMBER_OF_PREGENERATED_ACCOUNTS; ++i)
        {
            PublicKey publicKey;
            FuzzUtils::setShortKey(publicKey, i);

            auto createOp = txtest::createAccount(
                publicKey, FuzzUtils::INITIAL_ACCOUNT_BALANCE);

            ops.emplace_back(createOp);
        }

        attemptToApplyOps(ltx, root.getPublicKey(), ops.begin(), ops.end(),
                          *mApp);

        ltx.commit();
    }

    {
        LedgerTxn ltx(ltxOuter);

        xdr::xvector<Operation> ops;

        // For now we have every pregenerated account trust everything for some
        // assets issued by account indexed 1..NUMBER_OF_ASSETS_TO_ISSUE. We
        // also distribute some of these assets to everyone so that they can
        // make trades, payments, etc.
        for (uint8_t i = 0; i < FuzzUtils::NUMBER_OF_PREGENERATED_ACCOUNTS; ++i)
        {
            PublicKey account;
            FuzzUtils::setShortKey(account, i);

            for (int j = 1; j <= FuzzUtils::NUMBER_OF_ASSETS_TO_ISSUE; ++j)
            {
                auto const asset = FuzzUtils::makeAsset(j);
                if (i != j)
                {
                    // trust asset issuer
                    auto trustOp = txtest::changeTrust(
                        asset, FuzzUtils::INITIAL_TRUST_LINE_LIMIT);
                    trustOp.sourceAccount.activate() = toMuxedAccount(account);
                    ops.emplace_back(trustOp);

                    PublicKey issuer;
                    FuzzUtils::setShortKey(issuer, j);

                    // distribute asset
                    auto distributeOp = txtest::payment(
                        account, asset, FuzzUtils::INITIAL_ASSET_DISTRIBUTION);
                    distributeOp.sourceAccount.activate() =
                        toMuxedAccount(issuer);
                    ops.emplace_back(distributeOp);
                }
            }
        }

        attemptToApplyOps(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);

        ltx.commit();
    }

    {
        LedgerTxn ltx(ltxOuter);

        xdr::xvector<Operation> ops;

        for (auto const& param : orderBookParameters)
        {
            auto op = txtest::manageOffer(
                0, param.mBid.toAsset(), param.mSell.toAsset(),
                Price{param.mNumerator, param.mDenominator}, param.mAmount);
            PublicKey pkA;
            FuzzUtils::setShortKey(pkA, param.mPublicKey);
            op.sourceAccount.activate() = toMuxedAccount(pkA);
            ops.emplace_back(op);
        }

        attemptToApplyOps(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);

        ltx.commit();
    }

    {
        LedgerTxn ltx(ltxOuter);

        xdr::xvector<Operation> ops;

        // Reduce account balances so that fuzzing has a better chance
        // of exercising limits.
        for (uint8_t i = 0; i < FuzzUtils::NUMBER_OF_PREGENERATED_ACCOUNTS; ++i)
        {
            PublicKey account;
            FuzzUtils::setShortKey(account, i);

            // Reduce "account"'s native balance by paying the root.
            auto ae = stellar::loadAccount(ltx, account);
            auto const availableBalance =
                getAvailableBalance(ltx.loadHeader(), ae);
            auto const targetAvailableBalance =
                FuzzUtils::DEFAULT_ASSET_AVAILABLE_FOR_TEST_ACTIVITY +
                FuzzUtils::FUZZING_FEE *
                    FuzzUtils::DEFAULT_NUM_TRANSACTIONS_TO_RESERVE_FEES_FOR;

            if (availableBalance > targetAvailableBalance)
            {
                auto reduceNativeBalanceOp =
                    txtest::payment(mSourceAccountID,
                                    availableBalance - targetAvailableBalance);
                reduceNativeBalanceOp.sourceAccount.activate() =
                    toMuxedAccount(account);
                ops.emplace_back(reduceNativeBalanceOp);
            }

            for (uint8_t j = 1; j <= FuzzUtils::NUMBER_OF_ASSETS_TO_ISSUE; ++j)
            {
                auto const asset = FuzzUtils::makeAsset(j);
                if (i != j)
                {
                    PublicKey issuer;
                    FuzzUtils::setShortKey(issuer, j);

                    // Reduce "account"'s balance of asset "j" by paying the
                    // issuer.
                    auto tle = stellar::loadTrustLine(ltx, account, asset);
                    auto const availableTLBalance =
                        tle.getAvailableBalance(ltx.loadHeader());
                    auto const targetAvailableTLBalance =
                        FuzzUtils::DEFAULT_ASSET_AVAILABLE_FOR_TEST_ACTIVITY;

                    if (availableTLBalance > targetAvailableTLBalance)
                    {
                        auto reduceNonNativeBalanceOp = txtest::payment(
                            issuer, asset,
                            availableTLBalance - targetAvailableTLBalance);
                        reduceNonNativeBalanceOp.sourceAccount.activate() =
                            toMuxedAccount(account);
                        ops.emplace_back(reduceNonNativeBalanceOp);
                    }
                    // Here we could also reduce account "i"'s trust line limit
                    // for asset "j" (testing the buying liabilities to
                    // determine by how much).
                }
            }
        }

        attemptToApplyOps(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);

        ltx.commit();
    }

    // commit this to the ledger so that we have a starting, persistent
    // state to fuzz test against
    ltxOuter.commit();
}

void
TransactionFuzzer::shutdown()
{
    mApp->gracefulStop();
    while (mClock.crank(true))
    {
    }
}

void
TransactionFuzzer::inject(std::string const& filename)
{
    std::ifstream in;
    in.exceptions(std::ios::badbit);
    in.open(filename, std::ios::binary);

    xdr::xvector<Operation> ops;
    std::vector<char> bins(xdrSizeLimit());
    in.read(bins.data(), bins.size());
    auto actual = in.gcount();
    // stop if either
    // we could read the whole buffer (too much data was generated by the
    // fuzzer), or got a short read
    if (actual == xdrSizeLimit() || actual == 0)
    {
        return;
    }
    bins.resize(actual);
    try
    {
        xdr::xdr_from_fuzzer_opaque(bins, ops);
    }
    catch (std::exception const& e)
    {
        // in case of fuzzer creating an ill-formed xdr, generate an
        // xdr that will trigger a non-execution path so that the fuzzer
        // realizes it has hit an uninteresting case
        LOG_TRACE(DEFAULT_LOG,
                  "xdr::xdr_from_fuzzer_opaque() threw exception {}", e.what());
        return;
    }
    // limit operations per transaction to limit size of fuzzed input
    if (ops.size() < 1 || ops.size() > FuzzUtils::FUZZER_MAX_OPERATIONS)
    {
        LOG_TRACE(DEFAULT_LOG, "invalid ops.size() {}", ops.size());
        return;
    }

    resetTxInternalState(*mApp);
    LOG_TRACE(DEFAULT_LOG, "Fuzz ops ({}): {}", ops.size(), xdr_to_string(ops));
    LedgerTxn ltx(mApp->getLedgerTxnRoot());
    attemptToApplyOps(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp,
                      false);
}

int
TransactionFuzzer::xdrSizeLimit()
{
    // 50 bytes in compact mode seems to hold large operations
    return 50 * FuzzUtils::FUZZER_MAX_OPERATIONS;
}

#define FUZZER_INITIAL_CORPUS_OPERATION_GEN_UPPERBOUND 128
void
TransactionFuzzer::genFuzz(std::string const& filename)
{
    gRandomEngine.seed(std::random_device()());
    std::ofstream out;
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.open(filename, std::ofstream::binary | std::ofstream::trunc);
    autocheck::generator<Operation> gen;
    xdr::xvector<Operation> ops;
    ops.reserve(FuzzUtils::FUZZER_MAX_OPERATIONS);
    auto const numops = rand_uniform<int>(1, FuzzUtils::FUZZER_MAX_OPERATIONS);
    for (int i = 0; i < numops; ++i)
    {
        Operation op = gen(FUZZER_INITIAL_CORPUS_OPERATION_GEN_UPPERBOUND);
        // Use account 0 for the base cases as it's more likely to be useful
        // right away.
        if (!op.sourceAccount)
        {
            PublicKey a0;
            FuzzUtils::setShortKey(a0, 0);
            op.sourceAccount.activate() = toMuxedAccount(a0);
        }
        ops.emplace_back(op);
    }
    auto bins = xdr::xdr_to_fuzzer_opaque(ops);
    out.write(reinterpret_cast<char const*>(bins.data()), bins.size());
}

void
OverlayFuzzer::shutdown()
{
    mSimulation->stopAllNodes();
}

void
OverlayFuzzer::initialize()
{
    resetRandomSeed();
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    mSimulation = std::make_shared<Simulation>(Simulation::OVER_LOOPBACK,
                                               networkID, getFuzzConfig);

    SIMULATION_CREATE_NODE(10);
    SIMULATION_CREATE_NODE(11);

    SCPQuorumSet qSet0;
    qSet0.threshold = 2;
    qSet0.validators.push_back(v10NodeID);
    qSet0.validators.push_back(v11NodeID);

    mSimulation->addNode(v10SecretKey, qSet0);
    mSimulation->addNode(v11SecretKey, qSet0);

    mSimulation->addPendingConnection(v10SecretKey.getPublicKey(),
                                      v11SecretKey.getPublicKey());

    mSimulation->startAllNodes();

    // crank until nodes are connected
    mSimulation->crankUntil(
        [&]() {
            auto nodes = mSimulation->getNodes();
            auto numberOfSimulationConnections =
                nodes[ACCEPTOR_INDEX]
                    ->getOverlayManager()
                    .getAuthenticatedPeersCount() +
                nodes[INITIATOR_INDEX]
                    ->getOverlayManager()
                    .getAuthenticatedPeersCount();
            return numberOfSimulationConnections == 2;
        },
        std::chrono::milliseconds{500}, false);
}

void
OverlayFuzzer::inject(std::string const& filename)
{
    std::ifstream in;
    in.exceptions(std::ios::badbit);
    in.open(filename, std::ios::binary);

    StellarMessage msg;
    std::vector<char> bins(xdrSizeLimit());
    in.read(bins.data(), bins.size());
    auto actual = in.gcount();
    // if we could read the whole buffer, or got a short read, stop
    if (in || actual == 0)
    {
        return;
    }
    bins.resize(actual);
    try
    {
        xdr::xdr_from_fuzzer_opaque(bins, msg);
    }
    catch (...)
    {
        // in case of fuzzer creating an ill-formed xdr, generate an
        // xdr that will trigger a non-execution path so that the fuzzer
        // realizes it has hit an uninteresting case
        return;
    }

    if (isBadOverlayFuzzerInput(msg))
    {
        return;
    }

    auto nodeids = mSimulation->getNodeIDs();
    auto loopbackPeerConnection = mSimulation->getLoopbackConnection(
        nodeids[INITIATOR_INDEX], nodeids[ACCEPTOR_INDEX]);

    auto initiator = loopbackPeerConnection->getInitiator();
    auto acceptor = loopbackPeerConnection->getAcceptor();

    initiator->getApp().getClock().postAction(
        [initiator, msg]() { initiator->Peer::sendMessage(msg); }, "main",
        Scheduler::ActionType::NORMAL_ACTION);

    mSimulation->crankForAtMost(std::chrono::milliseconds{500}, false);

    // clear all queues and cancel all events
    initiator->clearInAndOutQueues();
    acceptor->clearInAndOutQueues();

    while (initiator->getApp().getClock().cancelAllEvents())
        ;
    while (acceptor->getApp().getClock().cancelAllEvents())
        ;
}

int
OverlayFuzzer::xdrSizeLimit()
{
    return MAX_MESSAGE_SIZE;
}

#define FUZZER_INITIAL_CORPUS_MESSAGE_GEN_UPPERBOUND 16
void
OverlayFuzzer::genFuzz(std::string const& filename)
{
    std::ofstream out;
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.open(filename, std::ofstream::binary | std::ofstream::trunc);
    autocheck::generator<StellarMessage> gen;
    StellarMessage m(gen(FUZZER_INITIAL_CORPUS_MESSAGE_GEN_UPPERBOUND));
    while (isBadOverlayFuzzerInput(m))
    {
        m = gen(FUZZER_INITIAL_CORPUS_MESSAGE_GEN_UPPERBOUND);
    }
    auto bins = xdr::xdr_to_fuzzer_opaque(m);
    out.write(reinterpret_cast<char const*>(bins.data()), bins.size());
}
}
