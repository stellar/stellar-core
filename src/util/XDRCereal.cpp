// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/XDRCereal.h"

void
cereal_override(cereal::JSONOutputArchive& ar, const stellar::PublicKey& s,
                const char* field)
{
    xdr::archive(ar, stellar::KeyUtils::toStrKey<stellar::PublicKey>(s), field);
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
cereal_override(cereal::JSONOutputArchive& ar, stellar::Asset const& asset,
                char const* field)
{
    if (asset.type() == stellar::ASSET_TYPE_NATIVE)
    {
        xdr::archive(ar, std::string("NATIVE"), field);
    }
    else
    {
        ar.setNextName(field);
        ar.startNode();
        xdr::archive(ar, stellar::assetToString(asset), "assetCode");
        xdr::archive(ar, stellar::getIssuer(asset), "issuer");
        ar.finishNode();
    }
}
