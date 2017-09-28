// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SignatureChecker.h"

#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "transactions/SignatureUtils.h"
#include "util/Algoritm.h"

namespace stellar
{

using xdr::operator<;
using xdr::operator==;

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
SignatureChecker::checkSignature(AccountID const& accountID,
                                 std::vector<Signer> const& signersV,
                                 int neededWeight)
{
    if (mProtocolVersion == 7)
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
            mUsedOneTimeSignerKeys[accountID].insert(signerKey.key);
            totalWeight += signerKey.weight;
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
                    totalWeight += signerKey.weight;
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

    verified = verifyAll(
        signers[SIGNER_KEY_TYPE_ED25519],
        [&](DecoratedSignature const& sig, Signer const& signerKey) {
            return SignatureUtils::verify(sig, signerKey.key, mContentsHash);
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
    if (mProtocolVersion == 7)
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

const UsedOneTimeSignerKeys&
SignatureChecker::usedOneTimeSignerKeys() const
{
    return mUsedOneTimeSignerKeys;
}
};
