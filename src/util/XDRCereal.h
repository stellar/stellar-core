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

using namespace std::placeholders;

template <uint32_t N>
void
cereal_override(cereal::JSONOutputArchive& ar, const xdr::opaque_array<N>& s,
                const char* field)
{
    xdr::archive(ar, stellar::binToHex(stellar::ByteSlice(s.data(), s.size())),
                 field);
}

// We still need one explicit composite-container override because cereal
// appears to process arrays-of-arrays internally, without calling back through
// an NVP adaptor.
template <uint32_t N>
void
cereal_override(cereal::JSONOutputArchive& ar,
                const xdr::xarray<stellar::Hash, N>& s, const char* field)
{
    std::vector<std::string> tmp;
    for (auto const& h : s)
    {
        tmp.emplace_back(
            stellar::binToHex(stellar::ByteSlice(h.data(), h.size())));
    }
    xdr::archive(ar, tmp, field);
}

template <uint32_t N>
void
cereal_override(cereal::JSONOutputArchive& ar, const xdr::opaque_vec<N>& s,
                const char* field)
{
    xdr::archive(ar, stellar::binToHex(stellar::ByteSlice(s.data(), s.size())),
                 field);
}
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
cereal_override(cereal::JSONOutputArchive& ar, const stellar::Asset& s,
                const char* field)
{
    xdr::archive(ar, stellar::assetToString(s), field);
}

template <typename T>
typename std::enable_if<xdr::xdr_traits<T>::is_enum>::type
cereal_override(cereal::JSONOutputArchive& ar, const T& t, const char* field)
{
    std::string name = xdr::xdr_traits<T>::enum_name(t);
    xdr::archive(ar, name, field);
}

template <typename T>
void
cereal_override(cereal::JSONOutputArchive& ar, const xdr::pointer<T>& t,
                const char* field)
{
    // We tolerate a little information-loss here collapsing *T into T for
    // the non-null case, and use JSON 'null' for the null case. This reads
    // much better than the thing JSONOutputArchive does with PtrWrapper.
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

// NOTE: Nothing else should include xdrpp/cereal.h directly.
// cereal_override's have to be defined before xdrpp/cereal.h,
// otherwise some interplay of name lookup and visibility
// during the enable_if call in the cereal adaptor fails to find them.
#include <xdrpp/cereal.h>
