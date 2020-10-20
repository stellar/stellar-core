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

using namespace std::placeholders;

template <uint32_t N>
void
cereal_override(cereal::JSONOutputArchive& ar, const xdr::xstring<N>& s,
                const char* field)
{
    xdr::archive(ar, static_cast<std::string const&>(s), field);
}

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

void cereal_override(cereal::JSONOutputArchive& ar, const stellar::PublicKey& s,
                     const char* field);

void cereal_override(cereal::JSONOutputArchive& ar,
                     const stellar::MuxedAccount& muxedAccount,
                     const char* field);

void cereal_override(cereal::JSONOutputArchive& ar, const stellar::Asset& s,
                     const char* field);

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

// If name is a nonempty string, the output string begins with it.
// If compact = true, the output string will not contain any indentation.
template <typename T>
std::string
xdr_to_string(const T& t, std::string const& name = "", bool compact = false)
{
    std::stringstream os;

    // Archives are designed to be used in an RAII manner and are guaranteed to
    // flush their contents only on destruction.
    {
        cereal::JSONOutputArchive ar(
            os, compact ? cereal::JSONOutputArchive::Options::NoIndent()
                        : cereal::JSONOutputArchive::Options::Default());
        if (!name.empty())
        {
            ar(cereal::make_nvp(name, t));
        }
        else
        {
            ar(t);
        }
    }
    return os.str();
}
