// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SignatureChecker.h"

#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "transactions/SignatureUtils.h"
#include "util/Algorithm.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include <Tracy.hpp>

namespace stellar
{
std::mutex SignatureChecker::gCheckValidOrApplyTxSigCacheMetricsMutex;
uint64_t SignatureChecker::gCheckValidOrApplyTxSigCacheHits = 0;
uint64_t SignatureChecker::gCheckValidOrApplyTxSigCacheLookups = 0;

SignatureChecker::SignatureChecker(
    uint32_t protocolVersion, Hash const& contentsHash,
    xdr::xvector<DecoratedSignature, 20> const& signatures)
    : mProtocolVersion{protocolVersion}
    , mContentsHash{contentsHash}
    , mSignatures{signatures}
{
    mUsedSignatures.resize(mSignatures.size());
}

bool
SignatureChecker::checkSignature(std::vector<Signer> const& signersV,
                                 int neededWeight)
{
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    return true;
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

    if (protocolVersionEquals(mProtocolVersion, ProtocolVersion::V_7))
    {
        return true;
    }

    auto signers =
        split(signersV, [](const Signer& s) { return s.key.type(); });

    // calculate the weight of the signatures
    int totalWeight = 0;

    // compare all available SIGNER_KEY_TYPE_PRE_AUTH_TX with current
    // transaction hash
    // current transaction hash is not stored in getEnvelope().signatures - it
    // is
    // computed with getContentsHash() method
    for (auto const& signerKey : signers[SIGNER_KEY_TYPE_PRE_AUTH_TX])
    {
        if (signerKey.key.preAuthTx() == mContentsHash)
        {
            auto w = signerKey.weight;
            if (protocolVersionStartsFrom(mProtocolVersion,
                                          ProtocolVersion::V_10) &&
                w > UINT8_MAX)
            {
                w = UINT8_MAX;
            }
            totalWeight += w;
            if (totalWeight >= neededWeight)
                return true;
        }
    }

    using VerifyT =
        std::function<bool(DecoratedSignature const&, Signer const&)>;
    auto verifyAll = [&](std::vector<Signer>& signers, VerifyT verify) {
        for (size_t i = 0; i < mSignatures.size(); i++)
        {
            auto const& sig = mSignatures[i];

            for (auto it = signers.begin(); it != signers.end(); ++it)
            {
                auto& signerKey = *it;
                if (verify(sig, signerKey))
                {
                    mUsedSignatures[i] = true;
                    auto w = signerKey.weight;
                    if (protocolVersionStartsFrom(mProtocolVersion,
                                                  ProtocolVersion::V_10) &&
                        w > UINT8_MAX)
                    {
                        w = UINT8_MAX;
                    }
                    totalWeight += w;
                    if (totalWeight >= neededWeight)
                        return true;

                    signers.erase(it);
                    break;
                }
            }
        }

        return false;
    };

    auto verified =
        verifyAll(signers[SIGNER_KEY_TYPE_HASH_X],
                  [&](DecoratedSignature const& sig, Signer const& signerKey) {
                      return SignatureUtils::verifyHashX(sig, signerKey.key);
                  });
    if (verified)
    {
        return true;
    }

    verified =
        verifyAll(signers[SIGNER_KEY_TYPE_ED25519],
                  [&](DecoratedSignature const& sig, Signer const& signerKey) {
                      auto [valid, cacheLookupRes] = SignatureUtils::verify(
                          sig, signerKey.key, mContentsHash);
                      updateTxSigCacheMetrics(cacheLookupRes);
                      return valid;
                  });
    if (verified)
    {
        return true;
    }

    verified = verifyAll(
        signers[SIGNER_KEY_TYPE_ED25519_SIGNED_PAYLOAD],
        [&](DecoratedSignature const& sig, Signer const& signerKey) {
            auto [valid, cacheLookupRes] =
                SignatureUtils::verifyEd25519SignedPayload(sig, signerKey.key);
            updateTxSigCacheMetrics(cacheLookupRes);
            return valid;
        });
    if (verified)
    {
        return true;
    }

    return false;
}

bool
SignatureChecker::checkAllSignaturesUsed() const
{
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    return true;
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

    if (protocolVersionEquals(mProtocolVersion, ProtocolVersion::V_7))
    {
        return true;
    }

    for (auto sigb : mUsedSignatures)
    {
        if (!sigb)
        {
            return false;
        }
    }
    return true;
}

std::pair<uint64_t, uint64_t>
SignatureChecker::flushTxSigCacheCounts()
{
    std::lock_guard<std::mutex> lock(gCheckValidOrApplyTxSigCacheMetricsMutex);
    auto res = std::make_pair(gCheckValidOrApplyTxSigCacheHits,
                              gCheckValidOrApplyTxSigCacheLookups);
    gCheckValidOrApplyTxSigCacheHits = 0;
    gCheckValidOrApplyTxSigCacheLookups = 0;
    return res;
}

void
SignatureChecker::disableCacheMetricsTracking()
{
    mTrackCacheMetrics = false;
}

void
SignatureChecker::updateTxSigCacheMetrics(
    PubKeyUtils::VerifySigCacheLookupResult cacheLookupRes)
{
    if (!mTrackCacheMetrics)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(gCheckValidOrApplyTxSigCacheMetricsMutex);
    if (cacheLookupRes != PubKeyUtils::VerifySigCacheLookupResult::NO_LOOKUP)
    {
        ++gCheckValidOrApplyTxSigCacheLookups;
    }

    if (cacheLookupRes == PubKeyUtils::VerifySigCacheLookupResult::HIT)
    {
        ++gCheckValidOrApplyTxSigCacheHits;
    }
}
};
