// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/SecretValue.h"

namespace stellar
{

bool
operator==(SecretValue const& x, SecretValue const& y)
{
    return x.value == y.value;
}

bool
operator!=(SecretValue const& x, SecretValue const& y)
{
    return !(x == y);
}
}
