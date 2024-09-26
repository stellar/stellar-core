#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-transaction.h"

namespace stellar
{

class Application;

// Returns true if the new entry being created has a valid proof (i.e. valid
// proof provided or no archival filter misses)
bool checkCreationProof(Application& app, LedgerKey const& lk,
                        xdr::xvector<ArchivalProof> const& proofs);

// Adds a creation proof for lk to proofs. Returns true if a proof was added or
// is not necessary. Returns false if no valid proof exists.
bool addCreationProof(Application& app, LedgerKey const& lk,
                      xdr::xvector<ArchivalProof>& proofs);

}