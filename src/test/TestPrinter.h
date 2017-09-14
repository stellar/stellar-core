#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-transaction.h"
#include <lib/catch.hpp>
#include <xdrpp/printer.h>

namespace Catch
{

template <>
std::string toString(stellar::Hash const& tr);

template <>
std::string toString(stellar::TransactionResult const& tr);
}
