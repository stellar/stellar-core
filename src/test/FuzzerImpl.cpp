// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/FuzzerImpl.h"
#include "invariant/OrderBookIsNotCrossed.h"
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
#include "transactions/MutableTransactionResult.h"
#include "transactions/OperationFrame.h"
#include "transactions/SignatureChecker.h"
#include "transactions/TransactionMetaFrame.h"
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
auto constexpr MIN_ACCOUNT_BALANCE =
    FUZZING_FEE * DEFAULT_NUM_TRANSACTIONS_TO_RESERVE_FEES_FOR;

// must be strictly less than 255
uint8_t constexpr NUMBER_OF_PREGENERATED_ACCOUNTS = 5U;

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
    default:
        throw std::runtime_error("Invalid Asset type");
    }
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
    default:
        throw std::runtime_error("Invalid AssetCode type");
    }
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
    case LIQUIDITY_POOL:
        return getShortKey(key.liquidityPool().liquidityPoolID);
    case CONFIG_SETTING:
        return static_cast<uint8_t>(key.configSetting().configSettingID);
    case CONTRACT_DATA:
        switch (key.contractData().contract.type())
        {
        case SC_ADDRESS_TYPE_ACCOUNT:
            return getShortKey(key.contractData().contract.accountId());
        case SC_ADDRESS_TYPE_CONTRACT:
            return key.contractData().contract.contractId().at(0);
        }
    case CONTRACT_CODE:
        return key.contractCode().hash.at(0);
    case TTL:
        return getShortKey(key.ttl().keyHash);
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

void
setShortKey(FuzzUtils::StoredPoolIDs const& storedPoolIDs, PoolID& key,
            uint8_t byte)
{
    key = storedPoolIDs[byte % NUM_STORED_POOL_IDS];
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
    stellar::FuzzUtils::StoredPoolIDs mStoredPoolIDs;
    std::uint8_t const* mCur;
    std::uint8_t const* const mEnd;

    xdr_fuzzer_unpacker(
        stellar::FuzzUtils::StoredLedgerKeys const& storedLedgerKeys,
        stellar::FuzzUtils::StoredPoolIDs const& storedPoolIDs,
        void const* start, void const* end)
        : mStoredLedgerKeys(storedLedgerKeys)
        , mStoredPoolIDs(storedPoolIDs)
        , mCur(reinterpret_cast<std::uint8_t const*>(start))
        , mEnd(reinterpret_cast<std::uint8_t const*>(end))
    {
        assert(mCur <= mEnd);
    }
    xdr_fuzzer_unpacker(
        stellar::FuzzUtils::StoredLedgerKeys const& storedLedgerKeys,
        stellar::FuzzUtils::StoredPoolIDs const& storedPoolIDs,
        msg_ptr const& m)
        : xdr_fuzzer_unpacker(storedLedgerKeys, storedPoolIDs, m->data(),
                              m->end())
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
    T
    get32()
    {
        // 1 byte --> uint32
        check(1);
        uint32_t w = get_byte();
        if (w == UINT8_MAX)
        {
            return std::numeric_limits<T>::max();
        }
        else if (w == UINT8_MAX - 1)
        {
            auto maxT = std::numeric_limits<T>::max();
            return xdr_traits<T>::from_uint(maxT - 1);
        }

        return xdr_traits<T>::from_uint(w);
    }

    template <typename T>
    T
    get64()
    {
        // 2 bytes --> uint64 **with** "sign extension"
        check(2);
        // load into a 16 signed
        int16_t w;
        get_bytes(&w, 2);
        // extend to 64 bit
        int64_t ww = w;
        if (ww == INT16_MAX)
        {
            return std::numeric_limits<T>::max();
        }
        else if (ww == INT16_MAX - 1)
        {
            return std::numeric_limits<T>::max() - 1;
        }

        return xdr_traits<T>::from_uint(ww);
    }

    template <typename T>
    typename std::enable_if<std::is_same<
        std::uint32_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T& t)
    {
        t = get32<T>();
    }

    template <typename T>
    typename std::enable_if<std::is_same<
        std::uint64_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T& t)
    {
        t = get64<T>();
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
         !std::is_same<stellar::LiquidityPoolDepositOp, T>::value &&
         !std::is_same<stellar::LiquidityPoolWithdrawOp, T>::value &&
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

    // PoolID is just an opaque vector of size 32, so we have to specialize the
    // deposit and withdraw ops instead
    template <typename T>
    typename std::enable_if<
        std::is_same<stellar::LiquidityPoolDepositOp, T>::value>::type
    operator()(T& depositOp)
    {
        check(1);
        auto v = get_byte();
        stellar::FuzzUtils::setShortKey(mStoredPoolIDs,
                                        depositOp.liquidityPoolID, v);

        depositOp.maxAmountA = get64<int64_t>();
        depositOp.maxAmountB = get64<int64_t>();

        auto minN = get32<int32_t>();
        auto minD = get32<int32_t>();
        auto maxN = get32<int32_t>();
        auto maxD = get32<int32_t>();

        depositOp.minPrice = stellar::Price{minN, minD};
        depositOp.maxPrice = stellar::Price{maxN, maxD};
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<stellar::LiquidityPoolWithdrawOp, T>::value>::type
    operator()(T& withdrawOp)
    {
        check(1);
        auto v = get_byte();
        stellar::FuzzUtils::setShortKey(mStoredPoolIDs,
                                        withdrawOp.liquidityPoolID, v);

        withdrawOp.amount = get64<int64_t>();
        withdrawOp.minAmountA = get64<int64_t>();
        withdrawOp.minAmountB = get64<int64_t>();
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
    stellar::FuzzUtils::StoredPoolIDs const& storedPoolIDs, Bytes const& m,
    Args&... args) -> decltype(detail::bytes_to_void(m))
{
    xdr_fuzzer_unpacker g(storedLedgerKeys, storedPoolIDs, m.data(),
                          m.data() + m.size());
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

static int RECURSION_COUNT = 0;
static const int RECURSION_LIMIT = 50;

template <>
void
generator_t::operator()<stellar::SCVal>(stellar::SCVal& val) const
{
    if (++RECURSION_COUNT > RECURSION_LIMIT)
    {
        stellar::SCVal v;
        val = v;
        return;
    }
    const auto& vals = stellar::SCVal::_xdr_case_values();
    stellar::SCValType v;

    uint32_t n = 0;
    (*this)(n);
    v = vals[n % vals.size()];

    val._xdr_discriminant(v, false);
    val._xdr_with_mem_ptr(field_archiver, v, *this, val, nullptr);
}

template <>
void
generator_t::operator()<stellar::SorobanAuthorizedInvocation>(
    stellar::SorobanAuthorizedInvocation& auth) const
{
    if (++RECURSION_COUNT > RECURSION_LIMIT)
    {
        stellar::SorobanAuthorizedInvocation a;
        auth = a;
        return;
    }

    xdr_traits<stellar::SorobanAuthorizedInvocation>::load(*this, auth);
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
    cfg.WORKER_THREADS = 2;
    cfg.QUORUM_INTERSECTION_CHECKER = false;
    cfg.PREFERRED_PEERS_ONLY = false;
    cfg.RUN_STANDALONE = true;
    cfg.TESTING_UPGRADE_DESIRED_FEE = FuzzUtils::FUZZING_FEE;
    cfg.TESTING_UPGRADE_RESERVE = FuzzUtils::FUZZING_RESERVE;

    return cfg;
}

static void
resetTxInternalState(Application& app)
{
    reinitializeAllGlobalStateWithSeed(1);
// reset caches to clear persistent state
#ifdef BUILD_TESTS
    app.getLedgerTxnRoot().resetForFuzzer();
    app.getInvariantManager().resetForFuzzer();
#endif // BUILD_TESTS
    app.getDatabase().clearPreparedStatementCache(
        app.getDatabase().getSession());
}

// FuzzTransactionFrame is a specialized TransactionFrame that includes
// useful methods for fuzzing such as an attemptApplication method for resetting
// ledger state and deterministically attempting application of transactions.
class FuzzTransactionFrame : public TransactionFrame
{
  private:
    MutableTxResultPtr mTxResult;

  public:
    FuzzTransactionFrame(Hash const& networkID,
                         TransactionEnvelope const& envelope)
        : TransactionFrame(networkID, envelope)
        , mTxResult(createSuccessResult()){};

    void
    attemptApplication(Application& app, AbstractLedgerTxn& ltx)
    {
        // No soroban ops allowed
        if (std::any_of(getOperations().begin(), getOperations().end(),
                        [](auto const& x) { return x->isSoroban(); }))
        {
            mTxResult->setResultCode(txFAILED);
            return;
        }

        // reset results of operations
        mTxResult = createSuccessResultWithFeeCharged(ltx.getHeader(), 0, true);

        // attempt application of transaction without processing the fee or
        // committing the LedgerTxn
        SignatureChecker signatureChecker{
            ltx.loadHeader().current().ledgerVersion, getContentsHash(),
            mEnvelope.v1().signatures};
        LedgerSnapshot ltxStmt(ltx);
        // if any ill-formed Operations, do not attempt transaction application
        auto isInvalidOperation = [&](auto const& op, auto& opResult) {
            return !op->checkValid(
                app.getAppConnector(), signatureChecker,
                app.getAppConnector().getSorobanNetworkConfigReadOnly(),
                ltxStmt, false, opResult, mTxResult->getSorobanData());
        };

        auto const& ops = getOperations();
        for (size_t i = 0; i < ops.size(); ++i)
        {
            auto const& op = ops[i];
            auto& opResult = mTxResult->getOpResultAt(i);
            if (isInvalidOperation(op, opResult))
            {
                mTxResult->setResultCode(txFAILED);
                return;
            }
        }

        // while the following method's result is not captured, regardless, for
        // protocols < 8, this triggered buggy caching, and potentially may do
        // so in the future
        loadSourceAccount(ltx, ltx.loadHeader());
        processSeqNum(ltx);
        TransactionMetaFrame tm(2);
        applyOperations(signatureChecker, app.getAppConnector(), ltx, tm,
                        *mTxResult, Hash{});
        if (mTxResult->getResultCode() == txINTERNAL_ERROR)
        {
            throw std::runtime_error("Internal error while fuzzing");
        }
    }

    TransactionResult&
    getResult()
    {
        return mTxResult->getResult();
    }

    TransactionResultCode
    getResultCode() const
    {
        return mTxResult->getResultCode();
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

std::array<
    AccountParameters,
    FuzzUtils::NUMBER_OF_PREGENERATED_ACCOUNTS> constexpr accountParameters{
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
      FuzzUtils::INITIAL_ACCOUNT_BALANCE -
          (FuzzUtils::MIN_ACCOUNT_BALANCE + (2 * FuzzUtils::FUZZING_RESERVE) +
           1),
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
     INT64_MAX - (FuzzUtils::MIN_ACCOUNT_BALANCE +
                  (2 * FuzzUtils::FUZZING_RESERVE) + 1),
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

void
TransactionFuzzer::initialize()
{
    reinitializeAllGlobalStateWithSeed(1);
    mApp = createTestApplication(mClock, getFuzzConfig(0));
    OrderBookIsNotCrossed::registerAndEnableInvariant(*mApp);
    auto root = TestAccount::createRoot(*mApp);
    mSourceAccountID = root.getPublicKey();

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

    // commit this to the ledger so that we have a starting, persistent
    // state to fuzz test against
    ltxOuter.commit();

#ifdef BUILD_TESTS
    mApp->getInvariantManager().snapshotForFuzzer();
#endif // BUILD_TESTS
}

void
TransactionFuzzer::storeSetupPoolIDs(AbstractLedgerTxn& ltx,
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
TransactionFuzzer::storeSetupLedgerKeysAndPoolIDs(AbstractLedgerTxn& ltx)
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
    // at most one "live" (modified) one:  the root account.
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

    // If we ever create more ledger entries during setup than we have room for
    // in mStoredLedgerEntries, then we will have to do something further.
    assert(init.size() <= FuzzUtils::NUM_VALIDATED_LEDGER_KEYS);

    // Store the ledger entries created during setup in mStoredLedgerKeys.
    auto firstGeneratedLedgerKey = std::transform(
        init.cbegin(), init.cend(), mStoredLedgerKeys.begin(), LedgerEntryKey);

    stellar::FuzzUtils::generateStoredLedgerKeys(firstGeneratedLedgerKey,
                                                 mStoredLedgerKeys.end());

    storeSetupPoolIDs(ltx, init);
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
        auto trustOp = txtest::changeTrust(
            asset, std::max<int64_t>(FuzzUtils::INITIAL_TRUST_LINE_LIMIT,
                                     trustLine.mAssetAvailableForTestActivity));

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

        if (!trustLine.mCallAllowTrustOp ||
            trustLine.mAllowTrustFlags & AUTHORIZED_FLAG)
        {
            // Distribute the starting amount of the asset (to be reduced after
            // orders have been placed).
            auto distributeOp = txtest::payment(
                account, asset,
                std::max<int64_t>(FuzzUtils::INITIAL_ASSET_DISTRIBUTION,
                                  trustLine.mAssetAvailableForTestActivity));
            distributeOp.sourceAccount.activate() = toMuxedAccount(issuer);
            ops.emplace_back(distributeOp);
        }
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
        FuzzUtils::emplaceConditionallySponsored(ops, op, param.mSponsored,
                                                 param.mSponsorKey, pkA);
    }

    applySetupOperations(ltx, mSourceAccountID, ops.begin(), ops.end(), *mApp);

    ltx.commit();
}

void
TransactionFuzzer::initializeLiquidityPools(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);

    xdr::xvector<Operation> ops;

    for (auto const& param : poolSetupParameters)
    {
        auto const trustor = param.mTrustor;
        PublicKey account;
        FuzzUtils::setShortKey(account, trustor);

        // First create the pool
        auto const assetA = param.mAssetA.toAsset();
        auto const assetB = param.mAssetB.toAsset();

        ChangeTrustAsset poolAsset;
        poolAsset.type(ASSET_TYPE_POOL_SHARE);
        poolAsset.liquidityPool().constantProduct().assetA = assetA;
        poolAsset.liquidityPool().constantProduct().assetB = assetB;
        poolAsset.liquidityPool().constantProduct().fee =
            LIQUIDITY_POOL_FEE_V18;

        auto trustOp = txtest::changeTrust(
            poolAsset, std::max<int64_t>(FuzzUtils::INITIAL_TRUST_LINE_LIMIT,
                                         param.mLimit));
        trustOp.sourceAccount.activate() = toMuxedAccount(account);
        FuzzUtils::emplaceConditionallySponsored(ops, trustOp, param.mSponsored,
                                                 param.mSponsorKey, account);

        // Then deposit
        if (param.mMaxAmountA > 0 && param.mMaxAmountB > 0)
        {
            auto depositOp = txtest::liquidityPoolDeposit(
                xdrSha256(poolAsset.liquidityPool()), param.mMaxAmountA,
                param.mMaxAmountB,
                Price{param.mMinPriceNumerator, param.mMinPriceDenominator},
                Price{param.mMaxPriceNumerator, param.mMaxPriceDenominator});
            depositOp.sourceAccount.activate() = toMuxedAccount(account);
            FuzzUtils::emplaceConditionallySponsored(
                ops, depositOp, param.mSponsored, param.mSponsorKey, account);
        }
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
            FuzzUtils::MIN_ACCOUNT_BALANCE;

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
TransactionFuzzer::adjustTrustLineBalancesAfterSetup(
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

void
TransactionFuzzer::shutdown()
{
    exit(1);
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
        xdr::xdr_from_fuzzer_opaque(mStoredLedgerKeys, mStoredPoolIDs, bins,
                                    ops);
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
              xdrToCerealString(ops, fmt::format("Fuzz ops ({})", ops.size())));

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
    reinitializeAllGlobalStateWithSeed(std::random_device()());
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
        if (op.body.type() == INVOKE_HOST_FUNCTION ||
            op.body.type() == EXTEND_FOOTPRINT_TTL ||
            op.body.type() == RESTORE_FOOTPRINT)
        {
            // Skip soroban txs for now because setting them up to be valid will
            // take some time.
            continue;
        }

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
    reinitializeAllGlobalStateWithSeed(1);
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
        xdr::xdr_from_fuzzer_opaque(mStoredLedgerKeys, mStoredPoolIDs, bins,
                                    msg);
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

    mSimulation->getNode(initiator->getPeerID())
        ->getClock()
        .postAction(
            [initiator, msg]() {
                initiator->Peer::sendMessage(
                    std::make_shared<StellarMessage const>(msg));
            },
            "main", Scheduler::ActionType::NORMAL_ACTION);

    mSimulation->crankForAtMost(std::chrono::milliseconds{500}, false);

    // clear all queues and cancel all events
    initiator->clearInAndOutQueues();
    acceptor->clearInAndOutQueues();

    while (mSimulation->getNode(initiator->getPeerID())
               ->getClock()
               .cancelAllEvents())
        ;
    while (mSimulation->getNode(acceptor->getPeerID())
               ->getClock()
               .cancelAllEvents())
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
    reinitializeAllGlobalStateWithSeed(std::random_device()());
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
