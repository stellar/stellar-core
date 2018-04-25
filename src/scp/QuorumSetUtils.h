// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "xdr/Stellar-SCP.h"

namespace stellar
{

bool isQuorumSetSane(SCPQuorumSet const& qSet, bool extraChecks);

// normalize the quorum set, optionally removing idToRemove
void normalizeQSet(SCPQuorumSet& qSet, NodeID const* idToRemove = nullptr);
}
