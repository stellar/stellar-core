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
    default:
        ar(cereal::make_nvp(field, addr));
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

#ifdef BUILD_TESTS
void
cereal_override(cereal::JSONInputArchive& ar, stellar::PublicKey& s,
                const char* field)
{
    std::string strkey;
    xdr::archive(ar, strkey, field);
    s = stellar::KeyUtils::fromStrKey<stellar::PublicKey>(strkey);
}

void
cereal_override(cereal::JSONInputArchive& ar, stellar::SCAddress& addr,
                char const* field)
{
    try
    {
        std::string strkey;
        xdr::archive(ar, strkey, field);
        uint8_t version;
        std::vector<uint8_t> decoded;
        if (stellar::strKey::fromStrKey(strkey, version, decoded))
        {
            switch (version)
            {
            case stellar::strKey::STRKEY_CONTRACT:
                addr.type(stellar::SC_ADDRESS_TYPE_CONTRACT);
                if (addr.contractId().size() != decoded.size())
                {
                    break;
                }
                std::copy(decoded.begin(), decoded.end(),
                          addr.contractId().begin());
                return;
            case stellar::strKey::STRKEY_PUBKEY_ED25519:
                addr.type(stellar::SC_ADDRESS_TYPE_ACCOUNT);
                addr.accountId().type(stellar::PUBLIC_KEY_TYPE_ED25519);
                if (addr.accountId().ed25519().size() != decoded.size())
                {
                    break;
                }
                std::copy(decoded.begin(), decoded.end(),
                          addr.accountId().ed25519().begin());
                return;
            }
        }
    }
    catch (...)
    {
    }
    ar(cereal::make_nvp(field, addr));
}

void
cereal_override(cereal::JSONInputArchive& ar, stellar::ConfigUpgradeSetKey& key,
                char const* field)
{
    ar.setNextName(field);
    ar.startNode();

    std::string id;
    xdr::archive(ar, id, "contractID");
    uint8_t version;
    std::vector<uint8_t> decoded;
    releaseAssertOrThrow(stellar::strKey::fromStrKey(id, version, decoded));
    releaseAssertOrThrow(version == stellar::strKey::STRKEY_CONTRACT);
    releaseAssertOrThrow(decoded.size() == key.contractID.size());
    std::copy(decoded.begin(), decoded.end(), key.contractID.begin());

    xdr::archive(ar, key.contentHash, "contentHash");

    ar.finishNode();
}

void
cereal_override(cereal::JSONInputArchive& ar,
                stellar::MuxedAccount& muxedAccount, char const* field)
{
    try
    {
        std::string key;
        xdr::archive(ar, key, field);
        uint8_t version;
        std::vector<uint8_t> decoded;
        releaseAssertOrThrow(
            stellar::strKey::fromStrKey(key, version, decoded));
        releaseAssertOrThrow(version == stellar::strKey::STRKEY_PUBKEY_ED25519);
        muxedAccount.type(stellar::KEY_TYPE_ED25519);
        releaseAssertOrThrow(decoded.size() == muxedAccount.ed25519().size());
        std::copy(decoded.begin(), decoded.end(),
                  muxedAccount.ed25519().begin());
    }
    catch (const cereal::RapidJSONException&)
    {
        std::string key;
        muxedAccount.type(stellar::KEY_TYPE_MUXED_ED25519);
        xdr::archive(
            ar,
            std::make_tuple(cereal::make_nvp("id", muxedAccount.med25519().id),
                            cereal::make_nvp("accountID", key)),
            field);
        muxedAccount.med25519().ed25519 =
            stellar::KeyUtils::fromStrKey<stellar::AccountID>(key).ed25519();
    }
}
#endif // BUILD_TESTS
