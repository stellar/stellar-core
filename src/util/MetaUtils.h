#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{
struct TransactionMeta;
struct TransactionResultMeta;
struct LedgerCloseMeta;
void normalizeMeta(LedgerCloseMeta& m);
void normalizeMeta(TransactionResultMeta& m);
void normalizeMeta(TransactionMeta& m);
}