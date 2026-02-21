// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#ifdef _XDRPP_CEREAL_H_HEADER_INCLUDED_
#error This header includes xdrpp/cereal.h after necessary definitions \
       so include this file instead of directly including xdrpp/cereal.h.
#endif

#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "transactions/TransactionUtils.h"
#include "util/types.h"
#include <cereal/archives/json.hpp>
#include <string>

using namespace std::placeholders;

template <uint32_t N>
void
cereal_override(cereal::JSONOutputArchive& ar, xdr::xstring<N> const& s,
                char const* field)
{
    // If we have any '\0's, we serialize as an object with a "raw" key so that
    // we can roundtrip it (cereal JSONInputArchive doesn't properly read in
    // '\0's
    bool hasZero = false;
    for (char ch : s)
    {
        if (ch == '\0')
        {
            hasZero = true;
            break;
        }
    }
    if (hasZero)
    {
        ar.setNextName(field);
        ar.startNode();
        ar(cereal::make_nvp("raw", stellar::binToHex(s)));
        ar.finishNode();
    }
    else
    {
        xdr::archive(ar, static_cast<std::string const&>(s), field);
    }
}

template <uint32_t N>
void
cereal_override(cereal::JSONOutputArchive& ar, xdr::opaque_array<N> const& s,
                char const* field)
{
    xdr::archive(ar, stellar::binToHex(stellar::ByteSlice(s.data(), s.size())),
                 field);
}

template <typename T>
std::enable_if_t<xdr::xdr_traits<T>::is_container>
cereal_override(cereal::JSONOutputArchive& ar, T const& t, char const* field)
{
    // CEREAL_SAVE_FUNCTION_NAME in include/cereal/archives/json.hpp runs
    // ar.setNextName() and ar(). ar() in turns calls process() in
    // include/cereal/cereal.hpp which calls prologue(), processImpl(),
    // epilogue(). We are imitating this behavior here by creating a sub-object
    // using prologue(), printing the content with xdr::archive, and finally
    // calling epilogue(). We must use xdr::archive instead of ar() because we
    // need to access the nested cereal_overrides.
    //
    // tl;dr This does what ar(cereal::make_nvp(...)) does while using nested
    // cereal_overrides.
    ar.setNextName(field);
    cereal::prologue(ar, t);

    // It does not matter what value we pass here to cereal::make_size_tag
    // since it will be ignored. See the comment
    //
    // > SizeTags are strictly ignored for JSON, they just indicate
    // > that the current node should be made into an array
    //
    // in include/cereal/archives/json.hpp
    ar(cereal::make_size_tag(0));
    for (auto const& element : t)
    {
        xdr::archive(ar, element);
    }
    cereal::epilogue(ar, t);
}

template <uint32_t N>
void
cereal_override(cereal::JSONOutputArchive& ar, xdr::opaque_vec<N> const& s,
                char const* field)
{
    xdr::archive(ar, stellar::binToHex(stellar::ByteSlice(s.data(), s.size())),
                 field);
}

void cereal_override(cereal::JSONOutputArchive& ar, stellar::PublicKey const& s,
                     char const* field);

void cereal_override(cereal::JSONOutputArchive& ar,
                     stellar::SCAddress const& addr, char const* field);

void cereal_override(cereal::JSONOutputArchive& ar,
                     stellar::ConfigUpgradeSetKey const& key,
                     char const* field);

void cereal_override(cereal::JSONOutputArchive& ar,
                     stellar::MuxedAccount const& muxedAccount,
                     char const* field);

void cerealPoolAsset(cereal::JSONOutputArchive& ar,
                     stellar::ChangeTrustAsset const& asset, char const* field);

void cerealPoolAsset(cereal::JSONOutputArchive& ar,
                     stellar::TrustLineAsset const& asset, char const* field);

template <typename T>
typename std::enable_if<std::is_same<stellar::Asset, T>::value ||
                        std::is_same<stellar::TrustLineAsset, T>::value ||
                        std::is_same<stellar::ChangeTrustAsset, T>::value>::type
cereal_override(cereal::JSONOutputArchive& ar, T const& asset,
                char const* field)
{
    switch (asset.type())
    {
    case stellar::ASSET_TYPE_NATIVE:
        xdr::archive(ar, std::string("NATIVE"), field);
        break;
    case stellar::ASSET_TYPE_POOL_SHARE:
        if constexpr (std::is_same_v<T, stellar::Asset>)
        {
            xdr::archive(ar, std::string("UNKNOWN"), field);
        }
        else
        {
            cerealPoolAsset(ar, asset, field);
        }
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
    {
        ar.setNextName(field);
        ar.startNode();

        // Cereal JSON archives can't handle roundtripping `\0`s, so if we have
        // an invalid assetCode, we serialize it as raw bytes
        auto archive = [&ar](auto& assetCode) {
            static_assert(std::is_same_v<std::decay_t<decltype(assetCode)>,
                                         stellar::AssetCode12> ||
                          std::is_same_v<std::decay_t<decltype(assetCode)>,
                                         stellar::AssetCode4>);
            int first0;
            for (first0 = 0; first0 < assetCode.size(); first0++)
            {
                if (!assetCode[first0])
                {
                    break;
                }
            }
            bool valid = true;
            for (int i = first0; i < assetCode.size(); i++)
            {
                if (assetCode[i])
                {
                    valid = false;
                    break;
                }
            }
            if (std::is_same_v<std::decay<decltype(assetCode)>,
                               stellar::AssetCode12> &&
                first0 <= 4)
            {
                valid = false;
            }
            if (valid)
            {
                std::string code;
                std::copy(assetCode.begin(), assetCode.begin() + first0,
                          std::back_inserter(code));
                xdr::archive(ar, code, "assetCode");
            }
            else
            {
                xdr::archive(ar, assetCode, "assetCodeRaw");
            }
        };
        std::string code;
        if (asset.type() == stellar::ASSET_TYPE_CREDIT_ALPHANUM4)
        {
            archive(asset.alphaNum4().assetCode);
        }
        else
        {
            archive(asset.alphaNum12().assetCode);
        }
        xdr::archive(ar, stellar::getIssuer(asset), "issuer");
        ar.finishNode();
        break;
    }
    default:
        xdr::archive(ar, std::string("UNKNOWN"), field);
    }
}

template <typename T>
typename std::enable_if<xdr::xdr_traits<T>::is_enum>::type
cereal_override(cereal::JSONOutputArchive& ar, T const& t, char const* field)
{
    auto const np = xdr::xdr_traits<T>::enum_name(t);
    std::string name;
    if (np != nullptr)
    {
        name = np;
    }
    else
    {
        name = std::to_string(t);
    }
    xdr::archive(ar, name, field);
}

template <typename T> constexpr bool isXdrPointer = false;
template <typename T> constexpr bool isXdrPointer<xdr::pointer<T>> = true;

template <typename T>
std::enable_if_t<!isXdrPointer<T>>
cereal_override(cereal::JSONOutputArchive& ar, xdr::pointer<T> const& t,
                char const* field)
{
    // We collapse *T into T for the non-null case, and use JSON 'null' for the
    // null case. This reads much better than the thing JSONOutputArchive does
    // with PtrWrapper and is clearer than the empty/1-element array that the
    // container override does. **T doesn't get collapsed so that we can tell
    // the difference between the outer and inner optionals being null (note
    // that, at the time of writing, none of the stellar xdr types are an
    // optional optional)
    if (t)
    {
        xdr::archive(ar, *t, field);
    }
    else
    {
        ar.setNextName(field);
        ar.writeName();
        ar.saveValue(nullptr);
    }
}

#ifdef BUILD_TESTS
// We shouldn't use the JSONInputArchive overrides in production code.

template <uint32_t N>
void
cereal_override(cereal::JSONInputArchive& ar, xdr::xstring<N>& s,
                const char* field)
{
    try
    {
        xdr::archive(ar, static_cast<std::string&>(s), field);
    }
    catch (cereal::RapidJSONException const&)
    {
        ar.setNextName(field);
        ar.startNode();
        std::string hex;
        ar(cereal::make_nvp("raw", hex));
        auto bin = stellar::hexToBin(hex);
        s.resize(bin.size());
        std::copy(bin.begin(), bin.end(), s.begin());
        ar.finishNode();
    }
}

template <uint32_t N>
void
cereal_override(cereal::JSONInputArchive& ar, xdr::opaque_array<N>& s,
                const char* field)
{
    std::string hex;
    xdr::archive(ar, hex, field);
    auto bin = stellar::hexToBin(hex);
    releaseAssertOrThrow(bin.size() == N);
    std::copy(bin.begin(), bin.end(), s.begin());
}

template <typename T>
std::enable_if_t<xdr::xdr_traits<T>::is_container>
cereal_override(cereal::JSONInputArchive& ar, T& t, const char* field)
{
    cereal::size_type size;
    ar.setNextName(field);
    cereal::prologue(ar, t);
    ar(cereal::make_size_tag(size));
    t.resize(static_cast<uint32_t>(size));
    for (auto&& element : t)
    {
        xdr::archive(ar, element);
    }
    cereal::epilogue(ar, t);
}

template <uint32_t N>
void
cereal_override(cereal::JSONInputArchive& ar, xdr::opaque_vec<N>& s,
                const char* field)
{
    std::string hex;
    xdr::archive(ar, hex, field);
    auto bin = stellar::hexToBin(hex);
    s.resize(bin.size());
    std::copy(bin.begin(), bin.end(), s.begin());
}

void cereal_override(cereal::JSONInputArchive& ar, stellar::PublicKey& s,
                     const char* field);

void cereal_override(cereal::JSONInputArchive& ar, stellar::SCAddress& addr,
                     const char* field);

void cereal_override(cereal::JSONInputArchive& ar,
                     stellar::ConfigUpgradeSetKey& key, const char* field);

void cereal_override(cereal::JSONInputArchive& ar,
                     stellar::MuxedAccount& muxedAccount, const char* field);

template <typename T>
typename std::enable_if<std::is_same<stellar::Asset, T>::value ||
                        std::is_same<stellar::TrustLineAsset, T>::value ||
                        std::is_same<stellar::ChangeTrustAsset, T>::value>::type
cereal_override(cereal::JSONInputArchive& ar, T& asset, const char* field)
{
    try
    {
        std::string name;
        xdr::archive(ar, name, field);
        if (name == "NATIVE")
        {
            asset.type(stellar::ASSET_TYPE_NATIVE);
            return;
        }
        if constexpr (std::is_same_v<stellar::TrustLineAsset, T>)
        {
            asset.type(stellar::ASSET_TYPE_POOL_SHARE);
            auto bin = stellar::hexToBin(name);
            if (bin.size() == asset.liquidityPoolID().size())
            {
                std::copy(bin.begin(), bin.end(),
                          asset.liquidityPoolID().begin());
                return;
            }
        }
    }
    catch (const cereal::RapidJSONException&)
    {
    }

    ar.setNextName(field);
    ar.startNode();
    if constexpr (std::is_same_v<stellar::ChangeTrustAsset, T>)
    {
        asset.type(stellar::ASSET_TYPE_POOL_SHARE);
        asset.liquidityPool().type(stellar::LIQUIDITY_POOL_CONSTANT_PRODUCT);
        auto& cp = asset.liquidityPool().constantProduct();

        try
        {

            xdr::archive(ar, cp.assetA, "assetA");
            xdr::archive(ar, cp.assetB, "assetB");
            xdr::archive(ar, cp.fee, "fee");
            ar.finishNode();
            return;
        }
        catch (cereal::Exception const&)
        {
        }
    }

    stellar::AccountID issuer;
    std::vector<uint8_t> assetCode;
    try
    {
        std::string code;
        xdr::archive(ar, code, "assetCode");
        std::copy(code.begin(), code.end(), std::back_inserter(assetCode));
    }
    catch (cereal::Exception const&)
    {
        std::string hex;
        xdr::archive(ar, hex, "assetCodeRaw");
        assetCode = stellar::hexToBin(hex);
        releaseAssertOrThrow(assetCode.size() == 12 || assetCode.size() == 4);
    }
    xdr::archive(ar, issuer, "issuer");
    ar.finishNode();

    auto setAlpha = [&assetCode, &issuer](auto& alpha) {
        alpha.assetCode.fill(0);
        std::copy(assetCode.begin(), assetCode.end(), alpha.assetCode.begin());
        alpha.issuer = issuer;
    };

    if (assetCode.size() <= 4)
    {
        asset.type(stellar::ASSET_TYPE_CREDIT_ALPHANUM4);
        setAlpha(asset.alphaNum4());
    }
    else
    {
        asset.type(stellar::ASSET_TYPE_CREDIT_ALPHANUM12);
        setAlpha(asset.alphaNum12());
    }
}

template <typename T>
typename std::enable_if<xdr::xdr_traits<T>::is_enum>::type
cereal_override(cereal::JSONInputArchive& ar, T& t, const char* field)
{
    using traits = xdr::xdr_traits<T>;
    using case_type = typename traits::case_type;
    thread_local std::unordered_map<std::string, case_type> map;

    if (map.empty())
    {
        for (auto& value : traits::enum_values())
        {
            case_type enum_value{value};
            map[traits::enum_name(static_cast<T>(value))] = enum_value;
        }
    }

    std::string name;
    xdr::archive(ar, name, field);
    auto val = map.find(name);
    if (val == map.end())
    {
        t = static_cast<T>(std::stoll(name));
    }
    else
    {
        t = static_cast<T>(val->second);
    }
}

template <typename T>
std::enable_if_t<!isXdrPointer<T>>
cereal_override(cereal::JSONInputArchive& ar, xdr::pointer<T>& t,
                const char* field)
{
    try
    {
        std::nullptr_t p;
        xdr::archive(ar, p, field);
        t.reset(nullptr);
    }
    catch (const cereal::RapidJSONException&)
    {
        T base;
        xdr::archive(ar, base, field);
        t = xdr::pointer<T>(new T{base});
    }
}
#endif // BUILD_TESTS

// NOTE: Nothing else should include xdrpp/cereal.h directly.
// cereal_override's have to be defined before xdrpp/cereal.h,
// otherwise some interplay of name lookup and visibility
// during the enable_if call in the cereal adaptor fails to find them.
#include <xdrpp/cereal.h>

namespace stellar
{
// If compact = true, the output string will not contain any indentation.
template <typename T>
std::string
xdrToCerealString(T const& t, std::string const& name, bool compact = false)
{
    std::stringstream os;

    // Archives are designed to be used in an RAII manner and are guaranteed to
    // flush their contents only on destruction.
    {
        cereal::JSONOutputArchive ar(
            os, compact ? cereal::JSONOutputArchive::Options::NoIndent()
                        : cereal::JSONOutputArchive::Options::Default());
        xdr::archive(ar, t, name.c_str());
    }
    return os.str();
}
}
