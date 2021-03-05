// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/FuzzerImpl.h"
#include "ledger/LedgerTxn.h"
#include "ledger/TrustLineWrapper.h"
#include "ledger/test/LedgerTestUtils.h"
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

#include <cstdint>
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
auto constexpr FUZZING_FEE = 1;
auto constexpr FUZZING_RESERVE = 4;
auto constexpr INITIAL_TRUST_LINE_LIMIT = 5 * INITIAL_ASSET_DISTRIBUTION;
auto constexpr DEFAULT_NUM_TRANSACTIONS_TO_RESERVE_FEES_FOR = 10;

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

uint8_t constexpr NUMBER_OF_ASSET_ISSUER_BITS = 5;
uint8_t constexpr NUMBER_OF_ASSET_CODE_BITS = 8 - NUMBER_OF_ASSET_ISSUER_BITS;
uint8_t constexpr NUMBER_OF_ASSETS_TO_USE = 1 << NUMBER_OF_ASSET_CODE_BITS;
uint8_t constexpr ENCODE_ASSET_CODE_MASK = NUMBER_OF_ASSETS_TO_USE - 1;

uint8_t
getShortKey(AssetCode4 const& code)
{
    return code.data()[0] & ENCODE_ASSET_CODE_MASK;
}

uint8_t
getShortKey(AssetCode12 const& code)
{
    return code.data()[0] & ENCODE_ASSET_CODE_MASK;
}

uint8_t
decodeAssetIssuer(uint8_t byte)
{
    return byte >> NUMBER_OF_ASSET_CODE_BITS;
}

uint8_t
decodeAssetCodeDigit(uint8_t byte)
{
    return byte & ENCODE_ASSET_CODE_MASK;
}

uint8_t
getShortKey(Asset const& asset)
{
    // This encoding does _not_ make compacting a left-inverse of unpack.  We
    // could make it so, but it's not necessary -- compacting, which alone uses
    // this function, is operating on a randomly-generated Asset anyway.
    switch (asset.type())
    {
    case ASSET_TYPE_NATIVE:
        return 0;
    case ASSET_TYPE_CREDIT_ALPHANUM4:
        return getShortKey(asset.alphaNum4().issuer);
    case ASSET_TYPE_CREDIT_ALPHANUM12:
        return getShortKey(asset.alphaNum12().issuer);
    }
    throw std::runtime_error("Invalid Asset type");
}

uint8_t
getShortKey(AssetCode const& code)
{
    switch (code.type())
    {
    case ASSET_TYPE_NATIVE:
        return 0;
    case ASSET_TYPE_CREDIT_ALPHANUM4:
        return getShortKey(code.assetCode4());
    case ASSET_TYPE_CREDIT_ALPHANUM12:
        return getShortKey(code.assetCode12());
    }
    throw std::runtime_error("Invalid AssetCode type");
}

uint8_t
getShortKey(ClaimableBalanceID const& balanceID)
{
    return balanceID.v0()[0];
}

uint8_t
getShortKey(LedgerKey const& key)
{
    switch (key.type())
    {
    case ACCOUNT:
        return getShortKey(key.account().accountID);
    case OFFER:
        return getShortKey(key.offer().sellerID);
    case TRUSTLINE:
        return getShortKey(key.trustLine().accountID);
    case DATA:
        return getShortKey(key.data().accountID);
    case CLAIMABLE_BALANCE:
        return getShortKey(key.claimableBalance().balanceID);
    }
    throw std::runtime_error("Unknown key type");
}

// Sets "code" to a 4-byte alphanumeric AssetCode "Ast<digit>".
void
setAssetCode4(AssetCode4& code, int digit)
{
    static_assert(
        FuzzUtils::NUMBER_OF_ASSETS_TO_USE <= 10,
        "asset code generation supports only single-digit asset numbers");
    assert(digit < FuzzUtils::NUMBER_OF_ASSETS_TO_USE);
    strToAssetCode(code, "Ast" + std::to_string(digit));
}

// For digit == 0, returns native Asset.
// For digit != 0, returns an Asset with a 4-byte alphanumeric code "Ast<digit>"
// and an issuer with the given public key.
Asset
makeAsset(int issuer, int digit)
{
    Asset asset;
    if (digit == 0)
    {
        asset.type(ASSET_TYPE_NATIVE);
    }
    else
    {
        asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        setAssetCode4(asset.alphaNum4().assetCode, digit);
        setShortKey(asset.alphaNum4().issuer, issuer);
    }
    return asset;
}

Asset
makeAsset(uint8_t byte)
{
    return makeAsset(decodeAssetIssuer(byte), decodeAssetCodeDigit(byte));
}

AssetCode
makeAssetCode(uint8_t byte)
{
    AssetCode code;
    auto digit = decodeAssetCodeDigit(byte);
    if (digit == 0)
    {
        code.type(ASSET_TYPE_NATIVE);
    }
    else
    {
        code.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        setAssetCode4(code.assetCode4(), digit);
    }
    return code;
}

void
generateStoredLedgerKeys(StoredLedgerKeys::iterator begin,
                         StoredLedgerKeys::iterator end)
{
    if (std::distance(begin, end) <= NUM_UNVALIDATED_LEDGER_KEYS)
    {
        throw std::runtime_error("No room for unvalidated ledger keys");
    }

    auto const firstUnvalidatedLedgerKey = end - NUM_UNVALIDATED_LEDGER_KEYS;

    // Generate valid ledger entry keys.
    std::generate(begin, firstUnvalidatedLedgerKey, []() {
        return LedgerEntryKey(LedgerTestUtils::generateValidLedgerEntry());
    });

    // Generate unvalidated ledger entry keys.
    std::generate(firstUnvalidatedLedgerKey, end, []() {
        size_t const entrySize = 3;
        return autocheck::generator<LedgerKey>()(entrySize);
    });
}

void
setShortKey(std::array<LedgerKey, NUM_STORED_LEDGER_KEYS> const& storedKeys,
            LedgerKey& key, uint8_t byte)
{
    key = storedKeys[byte % NUM_STORED_LEDGER_KEYS];
}

SequenceNumber
getSequenceNumber(AbstractLedgerTxn& ltx, PublicKey const& sourceAccountID)
{
    auto account = loadAccount(ltx, sourceAccountID);
    return account.current().data.account().seqNum;
}

// Append "newOp" to "ops", optionally after enclosing it in a sandwich of
// begin/end-sponsoring-future-reserves.
void
emplaceConditionallySponsored(xdr::xvector<Operation>& ops,
                              Operation const& newOp, bool isSponsored,
                              int sponsorShortKey,
                              PublicKey const& sponsoredKey)
{
    if (isSponsored)
    {
        PublicKey sponsorKey;
        FuzzUtils::setShortKey(sponsorKey, sponsorShortKey);

        auto beginSponsoringOp =
            txtest::beginSponsoringFutureReserves(sponsoredKey);
        beginSponsoringOp.sourceAccount.activate() = toMuxedAccount(sponsorKey);
        ops.emplace_back(beginSponsoringOp);
    }

    ops.emplace_back(newOp);

    if (isSponsored)
    {
        auto endSponsoringOp = txtest::endSponsoringFutureReserves();
        endSponsoringOp.sourceAccount.activate() = toMuxedAccount(sponsoredKey);
        ops.emplace_back(endSponsoringOp);
    }
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
    typename std::enable_if<
        (!std::is_same<stellar::AccountID, T>::value &&
         !std::is_same<stellar::MuxedAccount, T>::value &&
         !std::is_same<stellar::Asset, T>::value &&
         !std::is_same<stellar::AssetCode, T>::value &&
         !std::is_same<stellar::ClaimableBalanceID, T>::value &&
         !std::is_same<stellar::LedgerKey, T>::value) &&
        (xdr_traits<T>::is_class || xdr_traits<T>::is_container)>::type
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

    template <typename T>
    typename std::enable_if<std::is_same<stellar::Asset, T>::value>::type
    operator()(T const& asset)
    {
        // Convert Asset to 1 byte.
        check(1);
        auto b = stellar::FuzzUtils::getShortKey(asset);
        put_bytes(&b, 1);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::AssetCode, T>::value>::type
    operator()(T const& code)
    {
        // Convert AssetCode to 1 byte.
        check(1);
        auto b = stellar::FuzzUtils::getShortKey(code);
        put_bytes(&b, 1);
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<stellar::ClaimableBalanceID, T>::value>::type
    operator()(T const& balanceID)
    {
        // Convert ClaimableBalanceID to 1 byte for indexing into an array of
        // LedgerKeys that have been mentioned in the XDR of fuzzer operations.
        check(1);
        auto b = stellar::FuzzUtils::getShortKey(balanceID);
        put_bytes(&b, 1);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::LedgerKey, T>::value>::type
    operator()(T const& key)
    {
        // Convert LedgerKey to 1 byte for indexing into an array of LedgerKeys
        // that have been mentioned in the XDR of fuzzer operations.
        check(1);
        auto b = stellar::FuzzUtils::getShortKey(key);
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
    stellar::FuzzUtils::StoredLedgerKeys mStoredLedgerKeys;
    std::uint8_t const* mCur;
    std::uint8_t const* const mEnd;

    xdr_fuzzer_unpacker(
        stellar::FuzzUtils::StoredLedgerKeys const& storedLedgerKeys,
        void const* start, void const* end)
        : mStoredLedgerKeys(storedLedgerKeys)
        , mCur(reinterpret_cast<std::uint8_t const*>(start))
        , mEnd(reinterpret_cast<std::uint8_t const*>(end))
    {
        assert(mCur <= mEnd);
    }
    xdr_fuzzer_unpacker(
        stellar::FuzzUtils::StoredLedgerKeys const& storedLedgerKeys,
        msg_ptr const& m)
        : xdr_fuzzer_unpacker(storedLedgerKeys, m->data(), m->end())
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
    typename std::enable_if<
        (!std::is_same<stellar::AccountID, T>::value &&
         !std::is_same<stellar::MuxedAccount, T>::value &&
         !std::is_same<stellar::Asset, T>::value &&
         !std::is_same<stellar::AssetCode, T>::value &&
         !std::is_same<stellar::ClaimableBalanceID, T>::value &&
         !std::is_same<stellar::LedgerKey, T>::value) &&
        (xdr_traits<T>::is_class || xdr_traits<T>::is_container)>::type
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

    template <typename T>
    typename std::enable_if<std::is_same<stellar::Asset, T>::value>::type
    operator()(T& asset)
    {
        // 1 byte --> Asset
        check(1);
        std::uint8_t v = get_byte();
        asset = stellar::FuzzUtils::makeAsset(v);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::AssetCode, T>::value>::type
    operator()(T& code)
    {
        // 1 byte --> AssetCode
        check(1);
        std::uint8_t v = get_byte();
        code = stellar::FuzzUtils::makeAssetCode(v);
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<stellar::ClaimableBalanceID, T>::value>::type
    operator()(T& balanceID)
    {
        check(1);
        std::uint8_t v = get_byte();
        stellar::LedgerKey key;
        stellar::FuzzUtils::setShortKey(mStoredLedgerKeys, key, v);
        // If this one byte indexes a stored LedgerKey for a ClaimableBalanceID,
        // use that; otherwise just use the byte itself as the balance ID.
        if (key.type() == stellar::CLAIMABLE_BALANCE)
        {
            balanceID = key.claimableBalance().balanceID;
        }
        else
        {
            balanceID.type(stellar::CLAIMABLE_BALANCE_ID_TYPE_V0);
            balanceID.v0()[0] = v;
        }
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::LedgerKey, T>::value>::type
    operator()(T& key)
    {
        check(1);
        std::uint8_t v = get_byte();
        stellar::FuzzUtils::setShortKey(mStoredLedgerKeys, key, v);
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
xdr_from_fuzzer_opaque(
    stellar::FuzzUtils::StoredLedgerKeys const& storedLedgerKeys,
    Bytes const& m, Args&... args) -> decltype(detail::bytes_to_void(m))
{
    xdr_fuzzer_unpacker g(storedLedgerKeys, m.data(), m.data() + m.size());
    xdr_argpack_archive(g, args...);
    g.done();
}

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
template <>
void
generator_t::operator()(stellar::PublicKey& t) const
{
    // Generate account IDs in a somewhat larger range than the number of
    // accounts created during setup, so that the fuzzer can generate some
    // unused accounts (and also generate operation sequences in which it
    // creates a new account and then uses it in a later operation).
    uint8_t constexpr NUMBER_OF_ACCOUNT_IDS_TO_GENERATE = 32;
    static_assert(NUMBER_OF_ACCOUNT_IDS_TO_GENERATE >
                      stellar::FuzzUtils::NUMBER_OF_PREGENERATED_ACCOUNTS,
                  "Range of generated accounts too small");
    stellar::FuzzUtils::setShortKey(
        t.ed25519(), static_cast<uint8_t>(stellar::rand_uniform<int>(
                         0, NUMBER_OF_ACCOUNT_IDS_TO_GENERATE - 1)));
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
resetRandomSeed(unsigned int seed)
{
    // seed randomness
    srand(seed);
    gRandomEngine.seed(seed);
    autocheck::rng().seed(seed);
}

static void
resetTxInternalState(Application& app)
{
    resetRandomSeed(1);
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

        // attempt application of transaction without processing the fee or
        // committing the LedgerTxn
        SignatureChecker signatureChecker{
            ltx.loadHeader().current().ledgerVersion, getContentsHash(),
            mEnvelope.v1().signatures};
        // if any ill-formed Operations, do not attempt transaction application
        auto isInvalidOperation = [&](auto const& op) {
            return !op->checkValid(signatureChecker, ltx, false);
        };
        if (std::any_of(mOperations.begin(), mOperations.end(),
                        isInvalidOperation))
        {
            markResultFailed();
            return;
        }
        // while the following method's result is not captured, regardless, for
        // protocols < 8, this triggered buggy caching, and potentially may do
        // so in the future
        loadSourceAccount(ltx, ltx.loadHeader());
        processSeqNum(ltx);
        TransactionMeta tm(2);
        applyOperations(signatureChecker, app, ltx, tm);
        if (getResultCode() == txINTERNAL_ERROR)
        {
            throw std::runtime_error("Internal error while fuzzing");
        }
    }
};

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
    tx1.tx.seqNum = FuzzUtils::getSequenceNumber(ltx, sourceAccountID) + 1;
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

// Empties "ops" as operations are applied.  Throws if any operations fail.
// Handles breaking up the list of operations into multiple transactions, if the
// caller provides more operations than fit in a single transaction.
static void
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
            auto const msg = fmt::format(
                FMT_STRING("Error {} while setting up fuzzing -- "
                           "{}"),
                txFramePtr->getResultCode(),
                xdr_to_string(txFramePtr->getResult(), "TransactionResult"));
            LOG_FATAL(DEFAULT_LOG, "{}", msg);
            throw std::runtime_error(msg);
        }

        for (auto const& opFrame : txFramePtr->getOperations())
        {
            auto const& op = opFrame->getOperation();
            auto const& tr = opFrame->getResult().tr();
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
                    xdr_to_string(tr, "Operation"),
                    xdr_to_string(op, "Operation"));
                LOG_FATAL(DEFAULT_LOG, "{}", msg);
                throw std::runtime_error(msg);
            }
        }
    }
}

// Requires a set of operations small enough to fit in a single transaction.
// Tolerates the failure of transaction application.
static void
applyFuzzOperations(LedgerTxn& ltx, PublicKey const& sourceAccount,
                    xdr::xvector<Operation>::const_iterator begin,
                    xdr::xvector<Operation>::const_iterator end,
                    Application& app)
{
    auto txFramePtr = createFuzzTransactionFrame(ltx, sourceAccount, begin, end,
                                                 app.getNetworkID());
    txFramePtr->attemptApplication(app, ltx);
}

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
        assert(mSuffixDigit < FuzzUtils::NUMBER_OF_ASSETS_TO_USE);
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

std::array<
    AccountParameters,
    FuzzUtils::NUMBER_OF_PREGENERATED_ACCOUNTS> constexpr accountParameters{
    {{0, 256, 0},
     {1, 256, 0},
     {2, 256, 0, 1}, // sponsored by account 1
     {3, 256, AUTH_REQUIRED_FLAG},
     {4, 256, 0},
     {5, 256, 0},
     {6, 256, 0},
     {7, 256, 0},
     {8, 256, 0},
     {9, 256, 0},
     {10, 256, 0},
     {11, 256, 0},
     {12, 256, 0},
     {13, 256, 0},
     {14, 256, 0},
     {15, 256, 0}}};

struct TrustLineParameters : public SponsoredEntryParameters
{
    constexpr TrustLineParameters(int trustor, AssetID const& assetID,
                                  int64_t assetAvailableForTestActivity,
                                  int64_t spareLimitAfterSetup)
        : TrustLineParameters(trustor, assetID, assetAvailableForTestActivity,
                              spareLimitAfterSetup, false, 0)
    {
        assert(!mAssetID.mIsNative);
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
        assert(!mAssetID.mIsNative);
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
        assert(!mAssetID.mIsNative);
    }
};

std::array<TrustLineParameters, 60> constexpr trustLineParameters{
    {{0, AssetID(1), 256, 256},
     {0, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrust(0, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {0, AssetID(4), 256, 256},
     {1, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrust(1, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {1, AssetID(4), 256, 256},
     {2, AssetID(1), 256, 256},
     TrustLineParameters::withAllowTrust(2, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {2, AssetID(4), 256, 256},
     {3, AssetID(1), 256, 256},
     {3, AssetID(2), 256, 256},
     {3, AssetID(4), 256, 256},
     {4, AssetID(1), 256, 256},
     TrustLineParameters::withSponsor(4, AssetID(2), 256, 256,
                                      3), // sponsored by account 3
     TrustLineParameters::withAllowTrust(4, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {5, AssetID(1), 256, 256},
     {5, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrust(5, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {5, AssetID(4, 3), 256, 256},
     {6, AssetID(1), 256, 256},
     {6, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrust(6, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {6, AssetID(4), 256, 256},
     {7, AssetID(1), 256, 256},
     {7, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrust(7, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {7, AssetID(4), 256, 256},
     {8, AssetID(1), 256, 256},
     {8, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrust(8, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {8, AssetID(4), 256, 256},
     {9, AssetID(1), 256, 256},
     {9, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrust(9, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {9, AssetID(4), 256, 256},
     {10, AssetID(1), 256, 256},
     {10, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrust(10, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {10, AssetID(4), 256, 256},
     {11, AssetID(1), 256, 256},
     {11, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrustAndSponsor(
         11, AssetID(3), 256, 256, AUTHORIZED_FLAG,
         10), // sponsored by account 10
     {11, AssetID(4), 256, 256},
     {12, AssetID(1), 256, 256},
     {12, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrust(12, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     TrustLineParameters::withSponsor(12, AssetID(4), 256, 256,
                                      11), // sponsored by account 11
     {13, AssetID(1), 256, 256},
     {13, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrust(13, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {13, AssetID(4), 256, 256},
     {14, AssetID(1), 256, 256},
     {14, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrust(14, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {14, AssetID(4), 256, 256},
     {15, AssetID(1), 256, 256},
     {15, AssetID(2), 256, 256},
     TrustLineParameters::withAllowTrust(15, AssetID(3), 256, 256,
                                         AUTHORIZED_FLAG),
     {15, AssetID(4), 256, 256}}};

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

std::array<ClaimableBalanceParameters, 4> constexpr claimableBalanceParameters{{
    {0, 1, AssetID(), 10},     // native asset
    {2, 3, AssetID(4), 5},     // non-native asset
    {4, 5, AssetID(4), 20, 6}, // sponsored by account 6
    {4, 3, AssetID(3), 30}     // issuer is claimant
}};

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
// | 2          | A   | B    |    100 | 1/1 (passive)                |
// | 2          | B   | A    |    100 | 1/1 (passive)                |
// | 3 (issuer) | B   | A    |     10 | 10/9                         |
// | 4          | B   | A    |     50 | 10/9                         |
// | 0          | B   | A    |    100 | 22/7                         |
// +------------+-----+------+--------+------------------------------+
//
// (This is far more symmetric than it needs to be; we will introduce more
// variety.  In the long run, we plan to fuzz the setup itself.)
std::array<OfferParameters, 28> constexpr orderBookParameters{
    {{13, AssetID(), AssetID(1), 10, 3, 2, false}, // asset pair 0
     {14, AssetID(), AssetID(1), 50, 3, 2, false,
      15}, // sponsored by account 15
     {15, AssetID(), AssetID(1), 100, 1, 1, true},
     {15, AssetID(1), AssetID(), 100, 1, 1, true},
     {1, AssetID(1), AssetID(), 10, 10, 9, false},
     {12, AssetID(1), AssetID(), 50, 10, 9, false},
     {13, AssetID(1), AssetID(), 100, 22, 7, false},

     {11, AssetID(1), AssetID(2), 10, 3, 2, false}, // asset pair 1
     {1, AssetID(1), AssetID(2), 50, 3, 2, false},
     {12, AssetID(1), AssetID(2), 100, 1, 1, true},
     {12, AssetID(2), AssetID(1), 100, 1, 1, true},
     {2, AssetID(2), AssetID(1), 10, 10, 9, false},
     {10, AssetID(2), AssetID(1), 50, 10, 9, false},
     {11, AssetID(2), AssetID(1), 100, 22, 7, false},

     {13, AssetID(2), AssetID(3), 10, 3, 2, false}, // asset pair 2
     {2, AssetID(2), AssetID(3), 50, 3, 2, false},
     {14, AssetID(2), AssetID(3), 100, 1, 1, true},
     {14, AssetID(3), AssetID(2), 100, 1, 1, true},
     {3, AssetID(3), AssetID(2), 10, 10, 9, false},
     {15, AssetID(3), AssetID(2), 50, 10, 9, false},
     {13, AssetID(3), AssetID(2), 100, 22, 7, false},

     {6, AssetID(3), AssetID(4), 10, 3, 2, false}, // asset pair 3
     {3, AssetID(3), AssetID(4), 50, 3, 2, false},
     {7, AssetID(3), AssetID(4), 100, 1, 1, true},
     {7, AssetID(4), AssetID(3), 100, 1, 1, true},
     {4, AssetID(4), AssetID(3), 10, 10, 9, false},
     {8, AssetID(4), AssetID(3), 50, 10, 9, false},
     {6, AssetID(4), AssetID(3), 100, 22, 7, false}}};

void
TransactionFuzzer::initialize()
{
    resetRandomSeed(1);
    mApp = createTestApplication(mClock, getFuzzConfig(0));
    auto root = TestAccount::createRoot(*mApp);
    mSourceAccountID = root.getPublicKey();

    resetTxInternalState(*mApp);
    LedgerTxn ltxOuter(mApp->getLedgerTxnRoot());

    initializeAccounts(ltxOuter);

    initializeTrustLines(ltxOuter);

    initializeClaimableBalances(ltxOuter);

    initializeOffers(ltxOuter);

    reduceNativeBalancesAfterSetup(ltxOuter);

    reduceTrustLineBalancesAfterSetup(ltxOuter);

    reduceTrustLineLimitsAfterSetup(ltxOuter);

    storeSetupLedgerKeys(ltxOuter);

    // commit this to the ledger so that we have a starting, persistent
    // state to fuzz test against
    ltxOuter.commit();
}

void
TransactionFuzzer::storeSetupLedgerKeys(AbstractLedgerTxn& ltx)
{
    // Get the list of ledger entries created during setup to place into
    // mStoredLedgerKeys.
    std::vector<LedgerEntry> init, live;
    std::vector<LedgerKey> dead;
    ltx.getAllEntries(init, live, dead);

    // getAllEntries() does not guarantee anything about the order in which
    // entries are returned, so to minimize non-determinism in fuzzing setup, we
    // sort them.
    std::sort(init.begin(), init.end());

    // Setup should only create entries; there should be no dead entries, and
    // only one "live" (modified) one:  the root account.
    assert(dead.empty());
    assert(live.size() == 1);
    assert(live[0].data.type() == ACCOUNT);
    assert(live[0].data.account().accountID ==
           txtest::getRoot(mApp->getNetworkID()).getPublicKey());

    // If we ever create more ledger entries during setup than we have room for
    // in mStoredLedgerEntries, then we will have to do something further.
    assert(init.size() <= FuzzUtils::NUM_VALIDATED_LEDGER_KEYS);

    // Store the ledger entries created during setup in mStoredLedgerKeys.
    auto firstGeneratedLedgerKey = std::transform(
        init.cbegin(), init.cend(), mStoredLedgerKeys.begin(), LedgerEntryKey);

    stellar::FuzzUtils::generateStoredLedgerKeys(firstGeneratedLedgerKey,
                                                 mStoredLedgerKeys.end());
}

void
TransactionFuzzer::initializeAccounts(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);
    xdr::xvector<Operation> ops;

    for (auto const& param : accountParameters)
    {
        PublicKey publicKey;
        FuzzUtils::setShortKey(publicKey, param.mShortKey);

        FuzzUtils::emplaceConditionallySponsored(
            ops,
            txtest::createAccount(publicKey,
                                  FuzzUtils::INITIAL_ACCOUNT_BALANCE),
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
TransactionFuzzer::initializeTrustLines(AbstractLedgerTxn& ltxOuter)
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
        auto trustOp =
            txtest::changeTrust(asset, FuzzUtils::INITIAL_TRUST_LINE_LIMIT);
        trustOp.sourceAccount.activate() = toMuxedAccount(account);
        FuzzUtils::emplaceConditionallySponsored(
            ops, trustOp, trustLine.mSponsored, trustLine.mSponsorKey, account);

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

        // Distribute the starting amount of the asset (to be reduced after
        // orders have been placed).
        auto distributeOp = txtest::payment(
            account, asset, FuzzUtils::INITIAL_ASSET_DISTRIBUTION);
        distributeOp.sourceAccount.activate() = toMuxedAccount(issuer);
        ops.emplace_back(distributeOp);
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);

    ltx.commit();
}

void
TransactionFuzzer::initializeClaimableBalances(AbstractLedgerTxn& ltxOuter)
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
        FuzzUtils::emplaceConditionallySponsored(ops, claimableBalanceOp,
                                                 param.mSponsored,
                                                 param.mSponsorKey, senderKey);
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);

    ltx.commit();
}

void
TransactionFuzzer::initializeOffers(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);

    xdr::xvector<Operation> ops;

    for (auto const& param : orderBookParameters)
    {
        auto op = param.mPassive
                      ? txtest::createPassiveOffer(
                            param.mBid.toAsset(), param.mSell.toAsset(),
                            Price{param.mNumerator, param.mDenominator},
                            param.mAmount)
                      : txtest::manageOffer(
                            0, param.mBid.toAsset(), param.mSell.toAsset(),
                            Price{param.mNumerator, param.mDenominator},
                            param.mAmount);
        PublicKey pkA;
        FuzzUtils::setShortKey(pkA, param.mPublicKey);
        op.sourceAccount.activate() = toMuxedAccount(pkA);
        FuzzUtils::emplaceConditionallySponsored(ops, op, param.mSponsored,
                                                 param.mSponsorKey, pkA);
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);

    ltx.commit();
}

void
TransactionFuzzer::reduceNativeBalancesAfterSetup(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);

    xdr::xvector<Operation> ops;

    for (auto const& param : accountParameters)
    {
        PublicKey account;
        FuzzUtils::setShortKey(account, param.mShortKey);

        // Reduce "account"'s native balance by paying the root, so that
        // fuzzing has a better chance of exercising edge cases.
        auto ae = stellar::loadAccount(ltx, account);
        auto const availableBalance = getAvailableBalance(ltx.loadHeader(), ae);
        auto const targetAvailableBalance =
            param.mNativeAssetAvailableForTestActivity +
            FuzzUtils::FUZZING_FEE *
                FuzzUtils::DEFAULT_NUM_TRANSACTIONS_TO_RESERVE_FEES_FOR;

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
TransactionFuzzer::reduceTrustLineBalancesAfterSetup(
    AbstractLedgerTxn& ltxOuter)
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
        auto const availableTLBalance =
            tle.getAvailableBalance(ltx.loadHeader());
        auto const targetAvailableTLBalance =
            trustLine.mAssetAvailableForTestActivity;
        auto const paymentAmount =
            availableTLBalance - targetAvailableTLBalance;

        assert(availableTLBalance > targetAvailableTLBalance);
        auto reduceNonNativeBalanceOp =
            txtest::payment(issuer, asset, paymentAmount);
        reduceNonNativeBalanceOp.sourceAccount.activate() =
            toMuxedAccount(account);
        ops.emplace_back(reduceNonNativeBalanceOp);
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);

    ltx.commit();
}

void
TransactionFuzzer::reduceTrustLineLimitsAfterSetup(AbstractLedgerTxn& ltxOuter)
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
        auto const targetTrustLineLimit =
            tle.getBalance() + tle.getBuyingLiabilities(ltx.loadHeader()) +
            trustLine.mSpareLimitAfterSetup;

        auto changeTrustLineLimitOp =
            txtest::changeTrust(asset, targetTrustLineLimit);
        changeTrustLineLimitOp.sourceAccount.activate() =
            toMuxedAccount(account);
        ops.emplace_back(changeTrustLineLimitOp);
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);

    ltx.commit();
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
        xdr::xdr_from_fuzzer_opaque(mStoredLedgerKeys, bins, ops);
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
    LOG_TRACE(DEFAULT_LOG, "{}",
              xdr_to_string(ops, fmt::format("Fuzz ops ({})", ops.size())));

    LedgerTxn ltx(mApp->getLedgerTxnRoot());
    applyFuzzOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);
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
    resetRandomSeed(std::random_device()());
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
    resetRandomSeed(1);
    stellar::FuzzUtils::generateStoredLedgerKeys(mStoredLedgerKeys.begin(),
                                                 mStoredLedgerKeys.end());
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
        xdr::xdr_from_fuzzer_opaque(mStoredLedgerKeys, bins, msg);
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
    resetRandomSeed(std::random_device()());
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
