// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "crypto/SHA.h"
#include "crypto/ByteSlice.h"
#include <sodium.h>

namespace stellar
{

// Plain SHA256
uint256
sha256(ByteSlice const &bin)
{
    uint256 out;
    if (crypto_hash_sha256(out.data(), bin.data(), bin.size()) != 0)
        throw std::runtime_error("Error from crypto_hash_sha256");
    return out;
}

// Note: this function was omitted from libsodium, presumably for space,
// but to get sha512/256 exactly-as-specified in FIPS-180-4 we need to
// initialize the sha512 state with the "truncated variant" constants.
int
crypto_hash_sha512_256_init(crypto_hash_sha512_state *state)
{
    state->count[0] = state->count[1] = 0;

    state->state[0] = 0x22312194FC2BF72CULL;
    state->state[1] = 0x9F555FA3C84C64C2ULL;
    state->state[2] = 0x2393B86B6F53B151ULL;
    state->state[3] = 0x963877195940EABDULL;
    state->state[4] = 0x96283EE2A88EFFE3ULL;
    state->state[5] = 0xBE5E1E2553863992ULL;
    state->state[6] = 0x2B0199FC2C85B8AAULL;
    state->state[7] = 0x0EB72DDC81C52CA2ULL;

    return 0;
}

// SHA512/256: SHA512 truncated to 256 bits
uint256
sha512_256(ByteSlice const& bin)
{
    SHA512_256 s;
    s.add(bin);
    return s.finish();
}


struct SHA512_256::Impl
{
    crypto_hash_sha512_state mState;
    bool mFinished;

    Impl()
    : mFinished(false)
    {
            if (crypto_hash_sha512_256_init(&mState) != 0)
                throw std::runtime_error("error from crypto_hash_sha512_init");
    }
};

SHA512_256::SHA512_256()
    : mImpl(new Impl())
{
}

void
SHA512_256::add(ByteSlice const& bin)
{
    if (mImpl->mFinished)
        throw std::runtime_error("adding bytes to finished SHA512_256");
    if (crypto_hash_sha512_update(&mImpl->mState, bin.data(), bin.size()) != 0)
        throw std::runtime_error("error from crypto_hash_sha512_update");
}

uint256
SHA512_256::finish()
{
    unsigned char out[crypto_hash_sha512_BYTES];
    if (mImpl->mFinished)
        throw std::runtime_error("finishing already-finished SHA512_256");
    if (crypto_hash_sha512_final(&mImpl->mState, out) != 0)
        throw std::runtime_error("error from crypto_hash_sha512_final");
    uint256 trunc;
    std::copy(out, out+crypto_hash_sha256_BYTES, trunc.begin());
    return trunc;
}

}
