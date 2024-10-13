// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/XDRCereal.h"
#include "crypto/StrKey.h"
#include "xdr/Stellar-contract.h"

void
cereal_override(cereal::JSONOutputArchive& ar, const stellar::PublicKey& s,
                const char* field)
{
    xdr::archive(ar, stellar::KeyUtils::toStrKey<stellar::PublicKey>(s), field);
}

void
cereal_override(cereal::JSONOutputArchive& ar, const stellar::SCAddress& addr,
                const char* field)
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
    default:
        // this would be a bug
        abort();
    }
}

void
cereal_override(cereal::JSONOutputArchive& ar,
                const stellar::ConfigUpgradeSetKey& key, const char* field)
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
                const stellar::MuxedAccount& muxedAccount, const char* field)
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
cerealPoolAsset(cereal::JSONOutputArchive& ar, const stellar::Asset& asset,
                const char* field)
{
    xdr::archive(ar, std::string("INVALID"), field);
}

void
cerealPoolAsset(cereal::JSONOutputArchive& ar,
                const stellar::TrustLineAsset& asset, const char* field)
{
    cereal_override(ar, asset.liquidityPoolID(), field);
}

void
cerealPoolAsset(cereal::JSONOutputArchive& ar,
                const stellar::ChangeTrustAsset& asset, const char* field)
{
    auto const& cp = asset.liquidityPool().constantProduct();

    ar.setNextName(field);
    ar.startNode();

    xdr::archive(ar, cp.assetA, "assetA");
    xdr::archive(ar, cp.assetB, "assetB");

    xdr::archive(ar, cp.fee, "fee");
    ar.finishNode();
}
