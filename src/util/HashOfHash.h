#pragma once
#include <xdr/Stellar-types.h>

namespace std
{
template <> struct hash<stellar::uint256>
{
    size_t operator()(stellar::uint256 const& x) const noexcept;
};
}
