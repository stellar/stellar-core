// Copyright 2014-2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0


#include "crypto/KeyVerification.h"
#include "crypto/SHA.h"
#include "util/HashOfHash.h"
#include "util/RandomEvictionCache.h"
#include <mutex>
#include <sodium.h>

namespace stellar
{

// Process-wide global Ed25519 signature-verification cache.
//
// This is a pure mathematical function and has no relationship
// to the state of the process; caching its results centrally
// makes all signature-verification in the program faster and
// has no effect on correctness.

static std::mutex gVerifySigCacheMutex;
static RandomEvictionCache<Hash, bool> gVerifySigCache(0xffff);
static std::unique_ptr<SHA256> gHasher = SHA256::create();
static uint64_t gVerifyCacheHit = 0;
static uint64_t gVerifyCacheMiss = 0;

static Hash
verifySigCacheKey(PublicKey const& key, Signature const& signature,
                  ByteSlice const& bin)
{
    assert(key.type() == PUBLIC_KEY_TYPE_ED25519);

    gHasher->reset();
    gHasher->add(key.ed25519());
    gHasher->add(signature);
    gHasher->add(bin);
    return gHasher->finish();
}

bool
PubKeyUtils::verifySig(PublicKey const& key, Signature const& signature,
                       ByteSlice const& bin)
{
    assert(key.type() == PUBLIC_KEY_TYPE_ED25519);
    if (signature.size() != 64)
    {
        return false;
    }

    auto cacheKey = verifySigCacheKey(key, signature, bin);

    {
        std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
        if (gVerifySigCache.exists(cacheKey))
        {
            ++gVerifyCacheHit;
            return gVerifySigCache.get(cacheKey);
        }
    }

    ++gVerifyCacheMiss;
    bool ok =
        (crypto_sign_verify_detached(signature.data(), bin.data(), bin.size(),
                                     key.ed25519().data()) == 0);
    std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
    gVerifySigCache.put(cacheKey, ok);
    return ok;
}

void
PubKeyUtils::clearVerifySigCache()
{
    std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
    gVerifySigCache.clear();
}

void
PubKeyUtils::flushVerifySigCacheCounts(uint64_t& hits, uint64_t& misses)
{
    std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
    hits = gVerifyCacheHit;
    misses = gVerifyCacheMiss;
    gVerifyCacheHit = 0;
    gVerifyCacheMiss = 0;
}
}
