#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-types.h"

namespace stellar
{

class ByteSlice;
class SecretKey;
struct DecoratedSignature;

namespace SignatureUtils
{

DecoratedSignature sign(SecretKey const& secretKey, Hash const& hash);
DecoratedSignature signHashX(const ByteSlice &x);

}

}
