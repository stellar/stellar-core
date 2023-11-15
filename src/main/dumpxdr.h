#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include <functional>
#include <vector>

namespace stellar
{
void dumpXdrStream(std::string const& filename, bool json);
void printXdr(std::string const& filename, std::string const& filetype,
              bool base64, bool compact, bool rawMode);
void signtxns(std::vector<TransactionEnvelope>& txenvs, std::string netId,
              bool base64, bool txn_stdin, bool dump_hex_txid);
void signtxn(std::string const& filename, std::string netId, bool base64);
void priv2pub();
void readFile(const std::string& filename, bool base64,
              std::function<void(xdr::opaque_vec<>)> proc);
}
