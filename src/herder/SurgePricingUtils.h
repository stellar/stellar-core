#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionFrameBase.h"

namespace stellar
{
// 3-way fee rate compare variants
int feeRate3WayCompare(int64 lFeeBid, uint32 lNbOps, int64 rFeeBid,
                       uint32 rNbOps);

int feeRate3WayCompare(TransactionFrameBasePtr const& l,
                       TransactionFrameBasePtr const& r);

} // namespace stellar
