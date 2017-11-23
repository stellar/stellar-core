#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupWork.h"
#include "history/HistoryTestsUtils.h"
#include "lib/catch.hpp"
#include "xdr/Stellar-transaction.h"
#include "xdrpp/printer.h"

namespace stellar
{

struct OfferState;
}

namespace Catch
{

template <> std::string toString(stellar::ClaimOfferAtom const& coa);

template <> std::string toString(stellar::Hash const& tr);

template <> std::string toString(stellar::OfferEntry const& oe);

template <> std::string toString(stellar::OfferState const& os);

template <> std::string toString(stellar::TransactionResult const& tr);

template <> std::string toString(stellar::CatchupRange const& cr);

template <>
std::string toString(stellar::historytestutils::CatchupPerformedWork const& cm);
}
