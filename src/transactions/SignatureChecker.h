// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "crypto/SecretKey.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"
#include "xdr/Stellar-types.h"

#include <map>
#include <set>
#include <stdint.h>
#include <vector>

namespace stellar
{
class SignatureChecker
{
  public:
    // Construct a checker for validating `signatures` over `contentsHash`.
    explicit SignatureChecker(
        uint32_t protocolVersion, Hash const& contentsHash,
        xdr::xvector<DecoratedSignature, 20> const& signatures);
#ifdef BUILD_TESTS
    virtual bool checkSignature(std::vector<Signer> const& signersV,
                                int32_t neededWeight);
    virtual bool checkAllSignaturesUsed() const;
    virtual ~SignatureChecker() = default;

    // Do not record signature cache metrics for this instance. This should be
    // called for any signature validation that is not part of the checkValid or
    // apply flow.
    virtual void disableCacheMetricsTracking();
#else
    bool checkSignature(std::vector<Signer> const& signersV,
                        int32_t neededWeight);
    bool checkAllSignaturesUsed() const;

    // Do not record signature cache metrics for this instance. This should be
    // called for background transaction signature checking.
    void disableCacheMetricsTracking();
#endif // BUILD_TESTS

    // Reset and return the counts of signature checks performed as part of
    // transaction `checkValid` or apply flow. The first element of the pair is
    // the number of cache hits, and the second element is the total number of
    // cache lookups performed.
    static std::pair<uint64_t, uint64_t> flushTxSigCacheCounts();

  private:
    uint32_t mProtocolVersion;
    Hash const& mContentsHash;
    xdr::xvector<DecoratedSignature, 20> const& mSignatures;
    bool mTrackCacheMetrics{true};

    std::vector<bool> mUsedSignatures;

    // Static fields for tracking signature verification cache performance
    // during the `checkValid` or apply flow
    static std::mutex gCheckValidOrApplyTxSigCacheMetricsMutex;
    static uint64_t gCheckValidOrApplyTxSigCacheHits;
    static uint64_t gCheckValidOrApplyTxSigCacheLookups;

    // Given the result of a signature cache lookup, update the static metrics
    // counters if and only if this SignatureChecker is being used in the
    // `checkValid` or apply flow.
    void updateTxSigCacheMetrics(
        PubKeyUtils::VerifySigCacheLookupResult cacheLookupRes);
};

#ifdef BUILD_TESTS
class AlwaysValidSignatureChecker : public SignatureChecker
{
  public:
    AlwaysValidSignatureChecker(
        uint32_t protocolVersion, Hash const& contentsHash,
        xdr::xvector<DecoratedSignature, 20> const& signatures)
        : SignatureChecker(protocolVersion, contentsHash, signatures)
    {
    }

    bool
    checkSignature(std::vector<Signer> const&, int32_t) override
    {
        return true;
    }
    bool
    checkAllSignaturesUsed() const override
    {
        return true;
    }

    ~AlwaysValidSignatureChecker() override = default;
};
#endif // BUILD_TESTS
}
