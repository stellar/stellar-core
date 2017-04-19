#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/optional.h"
#include "xdr/Stellar-ledger-entries.h"

#include <cstdint>

namespace stellar
{

class Database;

void createSignersTable(Database& db);
xdr::xvector<Signer, 20u> loadSortedSigners(Database& db,
                                            std::string const& actIDStrKey);
void insertSigners(std::string actIDStrKey,
                   xdr::xvector<Signer, 20> const& signers, Database& db);
void updateSigners(std::string actIDStrKey,
                   xdr::xvector<Signer, 20> const& newSigners, Database& db);

optional<AccountID> selectSignerWithoutAccount(Database& db);
}
