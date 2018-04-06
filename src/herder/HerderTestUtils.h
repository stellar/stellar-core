#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-SCP.h"

namespace stellar
{
namespace HerderTestUtils
{

PublicKey makePublicKey(int i);
SCPQuorumSet makeSaneQuorumSet();
SCPQuorumSet makeBigQuorumSet();
SCPEnvelope makeEnvelope(Hash txHash, Hash qSetHash, uint64_t slotIndex);
}
}
