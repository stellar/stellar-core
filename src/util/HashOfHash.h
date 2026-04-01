// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <xdr/Stellar-types.h>

namespace std
{
template <> struct hash<stellar::uint256>
{
    size_t operator()(stellar::uint256 const& x) const noexcept;
};
}

namespace stellar
{
// FullHash256 feeds all 32 bytes of a uint256 to SipHash, providing
// collision resistance against adversarial inputs. Use this hasher for
// containers keyed on externally-supplied Hash values (e.g. transaction
// hashes received from peers).
struct FullHash256
{
    size_t operator()(stellar::uint256 const& x) const noexcept;
};
}
