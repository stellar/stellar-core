#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Always include this file instead of lib/catch.hpp in order to get access
// to Catch2.
// This is necessary for the StringMaker specializations to work properly
// without violating the one definition rule.
// Define any StringMaker specialzations here for pretty printing the custom
// types.

#include "lib/catch.hpp"
#include "util/XDRCereal.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-contract-config-setting.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-overlay.h"
#include "xdr/Stellar-transaction.h"
#include "xdr/Stellar-types.h"
#include "xdrpp/printer.h"
#include "xdrpp/types.h"

namespace stellar
{
struct OfferState;
class CatchupRange;

namespace historytestutils
{
struct CatchupPerformedWork;
} // namespace historytestutils
} // namespace stellar

namespace Catch
{
template <typename T>
struct StringMaker<T, std::enable_if_t<xdr::xdr_traits<T>::valid>>
{
    static std::string
    convert(T const& val)
    {
        return xdr::xdr_to_string(val, "value");
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

} // namespace Catch
