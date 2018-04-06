#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupWork.h"
#include "history/HistoryTestsUtils.h"
#include "lib/catch.hpp"
#include "xdrpp/printer.h"
#include "xdrpp/types.h"

namespace stellar
{
struct OfferState;
}

namespace Catch
{
template <typename T>
struct StringMaker<T, typename std::enable_if<xdr::xdr_traits<T>::valid>::type>
{
    static std::string
    convert(T const& val)
    {
        return xdr::xdr_to_string(val);
    }
};

template <> struct StringMaker<stellar::OfferState>
{
    static std::string convert(stellar::OfferState const& os);
};

template <> struct StringMaker<stellar::CatchupRange>
{
    static std::string convert(stellar::CatchupRange const& cr);
};

template <> struct StringMaker<stellar::historytestutils::CatchupPerformedWork>
{
    static std::string
    convert(stellar::historytestutils::CatchupPerformedWork const& cr);
};
}
