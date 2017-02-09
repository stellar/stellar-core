#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{

class ByteSlice;
class TransactionFrame;
struct SignerKey;

namespace SignerKeyUtils
{

SignerKey preAuthTxKey(TransactionFrame const& tx);
SignerKey hashXKey(ByteSlice const& bs);
}
}
