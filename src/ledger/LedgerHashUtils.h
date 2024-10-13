#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ShortHash.h"
#include "ledger/InternalLedgerEntry.h"
#include "util/HashOfHash.h"
#include "xdr/Stellar-ledger-entries.h"
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
        hashMix(res, std::hash<stellar::uint256>()(a4.issuer.ed25519()));
        hashMix(res, stellar::shortHash::computeHash(stellar::ByteSlice(
                         a4.assetCode.data(), a4.assetCode.size())));
        break;
    }
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
    {
        auto& a12 = asset.alphaNum12();
        hashMix(res, std::hash<stellar::uint256>()(a12.issuer.ed25519()));
        hashMix(res, stellar::shortHash::computeHash(stellar::ByteSlice(
                         a12.assetCode.data(), a12.assetCode.size())));
        break;
    }
    case stellar::ASSET_TYPE_POOL_SHARE:
    {
        hashMix(res, std::hash<stellar::uint256>()(getLiquidityPoolID(asset)));
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
            stellar::hashMix(res, std::hash<stellar::uint256>()(
                                      lk.account().accountID.ed25519()));
            break;
        case stellar::TRUSTLINE:
        {
            auto& tl = lk.trustLine();
            stellar::hashMix(
                res, std::hash<stellar::uint256>()(tl.accountID.ed25519()));
            stellar::hashMix(res, hash<stellar::TrustLineAsset>()(tl.asset));
            break;
        }
        case stellar::DATA:
            stellar::hashMix(res, std::hash<stellar::uint256>()(
                                      lk.data().accountID.ed25519()));
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
            stellar::hashMix(res, std::hash<stellar::uint256>()(
                                      lk.claimableBalance().balanceID.v0()));
            break;
        case stellar::LIQUIDITY_POOL:
            stellar::hashMix(res, std::hash<stellar::uint256>()(
                                      lk.liquidityPool().liquidityPoolID));
            break;
        case stellar::CONTRACT_DATA:
            switch (lk.contractData().contract.type())
            {
            case stellar::SC_ADDRESS_TYPE_ACCOUNT:
                stellar::hashMix(
                    res, std::hash<stellar::uint256>()(
                             lk.contractData().contract.accountId().ed25519()));
                break;
            case stellar::SC_ADDRESS_TYPE_CONTRACT:
                stellar::hashMix(res,
                                 std::hash<stellar::uint256>()(
                                     lk.contractData().contract.contractId()));
                break;
            }
            stellar::hashMix(
                res, stellar::shortHash::xdrComputeHash(lk.contractData().key));
            stellar::hashMix(
                res, std::hash<int32_t>()(lk.contractData().durability));
            break;
        case stellar::CONTRACT_CODE:
            stellar::hashMix(
                res, std::hash<stellar::uint256>()(lk.contractCode().hash));
            break;
        case stellar::CONFIG_SETTING:
            stellar::hashMix(
                res, std::hash<int32_t>()(lk.configSetting().configSettingID));
            break;
        case stellar::TTL:
            stellar::hashMix(res,
                             std::hash<stellar::uint256>()(lk.ttl().keyHash));
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
