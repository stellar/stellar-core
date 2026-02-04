// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/fuzz/FuzzUtils.h"

#include "main/Config.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/types.h"
#include "xdr/Stellar-transaction.h"

#include <algorithm>

namespace stellar
{

namespace FuzzUtils
{

void
setShortKey(uint256& key, int i)
{
    key.fill(0);
    key[0] = static_cast<unsigned char>(i);
}

void
setShortKey(PublicKey& key, int i)
{
    key.type(PUBLIC_KEY_TYPE_ED25519);
    setShortKey(key.ed25519(), i);
}

void
setShortKey(StoredLedgerKeys const& storedLedgerKeys, LedgerKey& key, uint8_t i)
{
    key = storedLedgerKeys[i % storedLedgerKeys.size()];
}

void
setShortKey(StoredPoolIDs const& storedPoolIDs, PoolID& id, uint8_t i)
{
    id = storedPoolIDs[i % storedPoolIDs.size()];
}

uint8_t
getShortKey(uint256 const& key)
{
    return key[0];
}

uint8_t
getShortKey(PublicKey const& key)
{
    return getShortKey(key.ed25519());
}

uint8_t
getShortKey(Asset const& asset)
{
    switch (asset.type())
    {
    case ASSET_TYPE_NATIVE:
        return 0;
    case ASSET_TYPE_CREDIT_ALPHANUM4:
        return asset.alphaNum4().assetCode[3];
    case ASSET_TYPE_CREDIT_ALPHANUM12:
        return asset.alphaNum12().assetCode[11];
    default:
        return 0;
    }
}

uint8_t
getShortKey(AssetCode const& code)
{
    switch (code.type())
    {
    case ASSET_TYPE_CREDIT_ALPHANUM4:
        return code.assetCode4()[3];
    case ASSET_TYPE_CREDIT_ALPHANUM12:
        return code.assetCode12()[11];
    default:
        return 0;
    }
}

uint8_t
getShortKey(ClaimableBalanceID const& balanceID)
{
    if (balanceID.type() == CLAIMABLE_BALANCE_ID_TYPE_V0)
    {
        return balanceID.v0()[0];
    }
    return 0;
}

uint8_t
getShortKey(LedgerKey const& key)
{
    // Use only the first byte for indexing.
    switch (key.type())
    {
    case ACCOUNT:
        return getShortKey(key.account().accountID);
    case TRUSTLINE:
        return getShortKey(key.trustLine().accountID);
    case OFFER:
        return static_cast<uint8_t>(key.offer().offerID);
    case DATA:
        return getShortKey(key.data().accountID);
    case CLAIMABLE_BALANCE:
        return getShortKey(key.claimableBalance().balanceID);
    case LIQUIDITY_POOL:
        return key.liquidityPool().liquidityPoolID[0];
    case CONTRACT_DATA:
    case CONTRACT_CODE:
    case CONFIG_SETTING:
    case TTL:
        return 0;
    }
    return 0;
}

Asset
makeAsset(int issuerShortKey, int assetCodeSuffix)
{
    Asset asset;
    if (assetCodeSuffix == 0)
    {
        asset.type(ASSET_TYPE_NATIVE);
    }
    else
    {
        asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        // Create asset code "Ast<digit>"
        strToAssetCode(asset.alphaNum4().assetCode,
                       "Ast" + std::to_string(assetCodeSuffix));
        setShortKey(asset.alphaNum4().issuer, issuerShortKey);
    }
    return asset;
}

Asset
makeAsset(int i)
{
    // asset code suffix and issuers index line up in the fuzzer setup; both are
    // `i`.
    return i == 0 ? txtest::makeNativeAsset() : makeAsset(i, i);
}

AssetCode
makeAssetCode(int i)
{
    AssetCode code;
    code.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    code.assetCode4()[3] = static_cast<char>(i);
    return code;
}

void
generateStoredLedgerKeys(StoredLedgerKeys::iterator begin,
                         StoredLedgerKeys::iterator end)
{
    std::generate(begin, end, []() {
        LedgerKey key;
        key.type(ACCOUNT);
        key.account().accountID.type(PUBLIC_KEY_TYPE_ED25519);
        key.account().accountID.ed25519().fill(0);
        return key;
    });
}

} // namespace FuzzUtils

// =============================================================================
// getFuzzConfig - creates a deterministic configuration for fuzzing
// =============================================================================

Config
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

} // namespace stellar

// =============================================================================
// XDR fuzzer compactor/unpacker - in xdr namespace
// =============================================================================

namespace xdr
{

using namespace stellar;

// xdr_fuzzer_compactor: Compresses XDR to a more compact format
xdr_fuzzer_compactor::xdr_fuzzer_compactor(void* start, void* end)
    : mStart(reinterpret_cast<std::uint8_t*>(start))
    , mCur(reinterpret_cast<std::uint8_t*>(start))
    , mEnd(reinterpret_cast<std::uint8_t*>(end))
{
    assert(mStart <= mEnd);
}

xdr_fuzzer_compactor::xdr_fuzzer_compactor(msg_ptr& m)
    : xdr_fuzzer_compactor(m->data(), m->end())
{
}

void
xdr_fuzzer_compactor::put_bytes(void const* buf, size_t len)
{
    if (len != 0)
    {
        std::memcpy(mCur, buf, len);
        mCur += len;
    }
}

void
xdr_fuzzer_compactor::check(std::size_t n) const
{
    if (n > std::size_t(reinterpret_cast<char*>(mEnd) -
                        reinterpret_cast<char*>(mCur)))
        throw xdr_overflow("insufficient buffer space in xdr_fuzzer_compactor");
}

uint32_t
xdr_fuzzer_compactor::size() const
{
    auto s = std::size_t(reinterpret_cast<char*>(mCur) -
                         reinterpret_cast<char*>(mStart));
    return static_cast<uint32_t>(s);
}

void
xdr_fuzzer_compactor::archive(stellar::AccountID const& pk)
{
    check(1);
    auto b = stellar::FuzzUtils::getShortKey(pk.ed25519());
    put_bytes(&b, 1);
}

void
xdr_fuzzer_compactor::archive(stellar::MuxedAccount const& m)
{
    auto const& ed25519 = (m.type() == stellar::KEY_TYPE_ED25519)
                              ? m.ed25519()
                              : m.med25519().ed25519;
    check(1);
    auto b = stellar::FuzzUtils::getShortKey(ed25519);
    put_bytes(&b, 1);
}

void
xdr_fuzzer_compactor::archive(stellar::Asset const& asset)
{
    check(1);
    auto b = stellar::FuzzUtils::getShortKey(asset);
    put_bytes(&b, 1);
}

void
xdr_fuzzer_compactor::archive(stellar::AssetCode const& code)
{
    check(1);
    auto b = stellar::FuzzUtils::getShortKey(code);
    put_bytes(&b, 1);
}

void
xdr_fuzzer_compactor::archive(stellar::ClaimableBalanceID const& balanceID)
{
    check(1);
    auto b = stellar::FuzzUtils::getShortKey(balanceID);
    put_bytes(&b, 1);
}

void
xdr_fuzzer_compactor::archive(stellar::LedgerKey const& key)
{
    check(1);
    auto b = stellar::FuzzUtils::getShortKey(key);
    put_bytes(&b, 1);
}

// xdr_fuzzer_unpacker: Expands compact format to XDR
xdr_fuzzer_unpacker::xdr_fuzzer_unpacker(
    stellar::FuzzUtils::StoredLedgerKeys const& storedLedgerKeys,
    stellar::FuzzUtils::StoredPoolIDs const& storedPoolIDs, void const* start,
    void const* end)
    : mStoredLedgerKeys(storedLedgerKeys)
    , mStoredPoolIDs(storedPoolIDs)
    , mCur(reinterpret_cast<std::uint8_t const*>(start))
    , mEnd(reinterpret_cast<std::uint8_t const*>(end))
{
    assert(mCur <= mEnd);
}

xdr_fuzzer_unpacker::xdr_fuzzer_unpacker(
    stellar::FuzzUtils::StoredLedgerKeys const& storedLedgerKeys,
    stellar::FuzzUtils::StoredPoolIDs const& storedPoolIDs, msg_ptr const& m)
    : xdr_fuzzer_unpacker(storedLedgerKeys, storedPoolIDs, m->data(), m->end())
{
}

void
xdr_fuzzer_unpacker::get_bytes(void* buf, size_t len)
{
    if (len != 0)
    {
        std::memcpy(buf, mCur, len);
        mCur += len;
    }
}

uint8_t
xdr_fuzzer_unpacker::get_byte()
{
    uint8_t b;
    get_bytes(&b, 1);
    return b;
}

void
xdr_fuzzer_unpacker::check(std::size_t n) const
{
    if (n > std::size_t(reinterpret_cast<char const*>(mEnd) -
                        reinterpret_cast<char const*>(mCur)))
        throw xdr_overflow("insufficient buffer space in xdr_fuzzer_unpacker");
}

void
xdr_fuzzer_unpacker::done()
{
    if (mCur != mEnd)
    {
        throw xdr_bad_message_size("trailing data in xdr_fuzzer_unpacker");
    }
}

void
xdr_fuzzer_unpacker::unarchive(stellar::AccountID& pk)
{
    check(1);
    std::uint8_t v = get_byte();
    stellar::FuzzUtils::setShortKey(pk, v);
}

void
xdr_fuzzer_unpacker::unarchive(stellar::MuxedAccount& m)
{
    check(1);
    std::uint8_t v = get_byte();
    stellar::FuzzUtils::setShortKey(m.ed25519(), v);
}

void
xdr_fuzzer_unpacker::unarchive(stellar::Asset& asset)
{
    check(1);
    std::uint8_t v = get_byte();
    asset = stellar::FuzzUtils::makeAsset(v);
}

void
xdr_fuzzer_unpacker::unarchive(stellar::AssetCode& code)
{
    check(1);
    std::uint8_t v = get_byte();
    code = stellar::FuzzUtils::makeAssetCode(v);
}

void
xdr_fuzzer_unpacker::unarchive(stellar::ClaimableBalanceID& balanceID)
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

void
xdr_fuzzer_unpacker::unarchive(stellar::LiquidityPoolDepositOp& depositOp)
{
    check(1);
    auto v = get_byte();
    stellar::FuzzUtils::setShortKey(mStoredPoolIDs, depositOp.liquidityPoolID,
                                    v);

    depositOp.maxAmountA = get64<int64_t>();
    depositOp.maxAmountB = get64<int64_t>();

    auto minN = get32<int32_t>();
    auto minD = get32<int32_t>();
    auto maxN = get32<int32_t>();
    auto maxD = get32<int32_t>();

    depositOp.minPrice = stellar::Price{minN, minD};
    depositOp.maxPrice = stellar::Price{maxN, maxD};
}

void
xdr_fuzzer_unpacker::unarchive(stellar::LiquidityPoolWithdrawOp& withdrawOp)
{
    check(1);
    auto v = get_byte();
    stellar::FuzzUtils::setShortKey(mStoredPoolIDs, withdrawOp.liquidityPoolID,
                                    v);

    withdrawOp.amount = get64<int64_t>();
    withdrawOp.minAmountA = get64<int64_t>();
    withdrawOp.minAmountB = get64<int64_t>();
}

void
xdr_fuzzer_unpacker::unarchive(stellar::LedgerKey& key)
{
    check(1);
    std::uint8_t v = get_byte();
    stellar::FuzzUtils::setShortKey(mStoredLedgerKeys, key, v);
}

} // namespace xdr
