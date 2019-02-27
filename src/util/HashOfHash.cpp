#include "HashOfHash.h"
#include "crypto/ByteSliceHasher.h"

namespace std
{

size_t
hash<stellar::uint256>::operator()(stellar::uint256 const& x) const noexcept
{
    size_t res =
        stellar::shortHash::computeHash(stellar::ByteSlice(x.data(), 8));

    return res;
}
}
