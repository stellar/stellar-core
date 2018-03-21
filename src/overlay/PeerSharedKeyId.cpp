// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerSharedKeyId.h"

namespace stellar
{

bool
operator==(PeerSharedKeyId const& x, PeerSharedKeyId const& y)
{
    return (x.mECDHPublicKey == y.mECDHPublicKey) && (x.mRole == y.mRole);
}

bool
operator!=(PeerSharedKeyId const& x, PeerSharedKeyId const& y)
{
    return !(x == y);
}
}

namespace std
{

size_t
hash<stellar::PeerSharedKeyId>::
operator()(stellar::PeerSharedKeyId const& x) const noexcept
{
    return std::hash<stellar::Curve25519Public>{}(x.mECDHPublicKey) ^
           std::hash<int>{}(static_cast<int>(x.mRole));
}
}
