#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ShortHash.h"
#include "ledger/InternalLedgerEntry.h"
#include "xdr/Stellar-ledger.h"
#include <functional>

namespace stellar
{

static PoolID const&
getLiquidityPoolID(Asset const& asset)
{
    throw std::runtime_error("cannot get PoolID from Asset");
}

static PoolID const&
getLiquidityPoolID(TrustLineAsset const& tlAsset)
{
    return tlAsset.liquidityPoolID();
}

static inline void
hashMix(size_t& h, size_t v)
{
    // from https://github.com/ztanml/fast-hash (MIT license)
    v ^= v >> 23;
    v *= 0x2127599bf4325c37ULL;
    v ^= v >> 47;
    h ^= v;
    h *= 0x880355f21e6d1965ULL;
}

template <typename T>
static size_t
getAssetHash(T const& asset)
{
    size_t res = asset.type();

    switch (asset.type())
    {
    case stellar::ASSET_TYPE_NATIVE:
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
    {
        auto& a4 = asset.alphaNum4();
        hashMix(res, stellar::shortHash::computeHash(
                         stellar::ByteSlice(a4.issuer.ed25519().data(), 8)));
        hashMix(res, stellar::shortHash::computeHash(stellar::ByteSlice(
                         a4.assetCode.data(), a4.assetCode.size())));
        break;
    }
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
    {
        auto& a12 = asset.alphaNum12();
        hashMix(res, stellar::shortHash::computeHash(
                         stellar::ByteSlice(a12.issuer.ed25519().data(), 8)));
        hashMix(res, stellar::shortHash::computeHash(stellar::ByteSlice(
                         a12.assetCode.data(), a12.assetCode.size())));
        break;
    }
    case stellar::ASSET_TYPE_POOL_SHARE:
    {
        hashMix(res, stellar::shortHash::computeHash(stellar::ByteSlice(
                         getLiquidityPoolID(asset).data(), 8)));
        break;
    }
    default:
        throw std::runtime_error("unknown Asset type");
    }
    return res;
}

}

// implements a default hasher for "LedgerKey"
namespace std
{
template <> class hash<stellar::Asset>
{
  public:
    size_t
    operator()(stellar::Asset const& asset) const
    {
        return stellar::getAssetHash<stellar::Asset>(asset);
    }
};

template <> class hash<stellar::TrustLineAsset>
{
  public:
    size_t
    operator()(stellar::TrustLineAsset const& asset) const
    {
        return stellar::getAssetHash<stellar::TrustLineAsset>(asset);
    }
};

template <> class hash<stellar::LedgerKey>
{
  public:
    size_t
    operator()(stellar::LedgerKey const& lk) const
    {
        size_t res = lk.type();
        switch (lk.type())
        {
        case stellar::ACCOUNT:
            stellar::hashMix(res,
                             stellar::shortHash::computeHash(stellar::ByteSlice(
                                 lk.account().accountID.ed25519().data(), 8)));
            break;
        case stellar::TRUSTLINE:
        {
            auto& tl = lk.trustLine();
            stellar::hashMix(
                res, stellar::shortHash::computeHash(
                         stellar::ByteSlice(tl.accountID.ed25519().data(), 8)));
            stellar::hashMix(res, hash<stellar::TrustLineAsset>()(tl.asset));
            break;
        }
        case stellar::DATA:
            stellar::hashMix(res,
                             stellar::shortHash::computeHash(stellar::ByteSlice(
                                 lk.data().accountID.ed25519().data(), 8)));
            stellar::hashMix(
                res,
                stellar::shortHash::computeHash(stellar::ByteSlice(
                    lk.data().dataName.data(), lk.data().dataName.size())));
            break;
        case stellar::OFFER:
            stellar::hashMix(
                res, stellar::shortHash::computeHash(stellar::ByteSlice(
                         &lk.offer().offerID, sizeof(lk.offer().offerID))));
            break;
        case stellar::CLAIMABLE_BALANCE:
            stellar::hashMix(
                res, stellar::shortHash::computeHash(stellar::ByteSlice(
                         lk.claimableBalance().balanceID.v0().data(), 8)));
            break;
        case stellar::LIQUIDITY_POOL:
            stellar::hashMix(
                res, stellar::shortHash::computeHash(stellar::ByteSlice(
                         lk.liquidityPool().liquidityPoolID.data(), 8)));
            break;
        default:
            abort();
        }
        return res;
    }
};

template <> class hash<stellar::InternalLedgerKey>
{
  public:
    size_t
    operator()(stellar::InternalLedgerKey const& glk) const
    {
        return glk.hash();
    }
};
}
