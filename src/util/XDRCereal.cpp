// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/XDRCereal.h"
#include "crypto/StrKey.h"
#include "xdr/Stellar-contract.h"

void
cereal_override(cereal::JSONOutputArchive& ar, stellar::PublicKey const& s,
                char const* field)
{
    xdr::archive(ar, stellar::KeyUtils::toStrKey<stellar::PublicKey>(s), field);
}

void
cereal_override(cereal::JSONOutputArchive& ar, stellar::SCAddress const& addr,
                char const* field)
{
    switch (addr.type())
    {
    case stellar::SC_ADDRESS_TYPE_CONTRACT:
        xdr::archive(ar,
                     stellar::strKey::toStrKey(stellar::strKey::STRKEY_CONTRACT,
                                               addr.contractId())
                         .value,
                     field);
        return;
    case stellar::SC_ADDRESS_TYPE_ACCOUNT:
        xdr::archive(ar, stellar::KeyUtils::toStrKey(addr.accountId()), field);
        return;
    case stellar::SC_ADDRESS_TYPE_MUXED_ACCOUNT:
        ar.setNextName(field);
        ar.startNode();
        xdr::archive(ar, addr.muxedAccount().id, "id");
        xdr::archive(ar,
                     stellar::binToHex(stellar::ByteSlice(
                         addr.muxedAccount().ed25519.data(),
                         addr.muxedAccount().ed25519.size())),
                     "ed25519");
        ar.finishNode();
        return;
    case stellar::SC_ADDRESS_TYPE_CLAIMABLE_BALANCE:
    {
        auto const& cbID = addr.claimableBalanceId();
        if (cbID.type() == stellar::CLAIMABLE_BALANCE_ID_TYPE_V0)
        {
            xdr::archive(ar,
                         stellar::binToHex(stellar::ByteSlice(
                             cbID.v0().data(), cbID.v0().size())),
                         field);
        }
        return;
    }
    case stellar::SC_ADDRESS_TYPE_LIQUIDITY_POOL:
        xdr::archive(
            ar,
            stellar::binToHex(stellar::ByteSlice(
                addr.liquidityPoolId().data(), addr.liquidityPoolId().size())),
            field);
        return;
    default:
        // Unknown address type - serialize as "Unknown(type_id)"
        xdr::archive(ar,
                     std::string("Unknown(") +
                         std::to_string(static_cast<int>(addr.type())) + ")",
                     field);
        return;
    }
}

void
cereal_override(cereal::JSONOutputArchive& ar,
                stellar::ConfigUpgradeSetKey const& key, char const* field)
{
    ar.setNextName(field);
    ar.startNode();

    xdr::archive(ar,
                 stellar::strKey::toStrKey(stellar::strKey::STRKEY_CONTRACT,
                                           key.contractID)
                     .value,
                 "contractID");
    xdr::archive(ar, key.contentHash, "contentHash");

    ar.finishNode();
}

void
cereal_override(cereal::JSONOutputArchive& ar,
                stellar::MuxedAccount const& muxedAccount, char const* field)
{
    switch (muxedAccount.type())
    {
    case stellar::KEY_TYPE_ED25519:
        xdr::archive(ar, stellar::KeyUtils::toStrKey(toAccountID(muxedAccount)),
                     field);
        return;
    case stellar::KEY_TYPE_MUXED_ED25519:
        xdr::archive(
            ar,
            std::make_tuple(
                cereal::make_nvp("id", muxedAccount.med25519().id),
                cereal::make_nvp("accountID", stellar::KeyUtils::toStrKey(
                                                  toAccountID(muxedAccount)))),
            field);
        return;
    default:
        // this would be a bug
        abort();
    }
}

void
cerealPoolAsset(cereal::JSONOutputArchive& ar, stellar::Asset const& asset,
                char const* field)
{
    xdr::archive(ar, std::string("INVALID"), field);
}

void
cerealPoolAsset(cereal::JSONOutputArchive& ar,
                stellar::TrustLineAsset const& asset, char const* field)
{
    cereal_override(ar, asset.liquidityPoolID(), field);
}

void
cerealPoolAsset(cereal::JSONOutputArchive& ar,
                stellar::ChangeTrustAsset const& asset, char const* field)
{
    auto const& cp = asset.liquidityPool().constantProduct();

    ar.setNextName(field);
    ar.startNode();

    xdr::archive(ar, cp.assetA, "assetA");
    xdr::archive(ar, cp.assetB, "assetB");

    xdr::archive(ar, cp.fee, "fee");
    ar.finishNode();
}
