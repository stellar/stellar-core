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

template <typename T>
std::enable_if_t<xdr::xdr_traits<T>::is_container>
cereal_override(cereal::JSONOutputArchive& ar, T const& t, const char* field)
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

// If compact = true, the output string will not contain any indentation.
template <typename T>
std::string
xdr_to_string(const T& t, std::string const& name, bool compact = false)
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
