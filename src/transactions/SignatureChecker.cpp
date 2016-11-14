// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SignatureChecker.h"

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "util/Algoritm.h"

namespace stellar
{

using xdr::operator < ;
using xdr::operator == ;

SignatureChecker::SignatureChecker(Hash const &contentsHash, xdr::xvector<DecoratedSignature,20> const &signatures) :
    mContentsHash(contentsHash),
    mSignatures(signatures)
{
    mUsedSignatures.resize(mSignatures.size());
}

bool SignatureChecker::checkSignature(AccountID const &accountID, std::vector<Signer> const &signersV, int neededWeight)
{
    auto signers = split(signersV, [](const Signer &s){ return s.key.type(); });

    // calculate the weight of the signatures
    int totalWeight = 0;

    // compare all available SIGNER_KEY_TYPE_HASH_TX with current transaction hash
    // current transaction hash is not stored in getEnvelope().signatures - it is
    // computed with getContentsHash() method
    for (auto const &signerKey : signers[SIGNER_KEY_TYPE_HASH_TX])
    {
        if (signerKey.key.hashTx() == mContentsHash)
        {
            mUsedOneTimeSignerKeys[accountID].insert(signerKey.key);
            totalWeight += signerKey.weight;
            if (totalWeight >= neededWeight)
                return true;
        }
    }

    auto &accountKeyWeights = signers[SIGNER_KEY_TYPE_ED25519];
    for (size_t i = 0; i < mSignatures.size(); i++)
    {
        auto const& sig = mSignatures[i];

        for (auto it = accountKeyWeights.begin(); it != accountKeyWeights.end(); ++it)
        {
            auto &signerKey = *it;
            auto pubKey = KeyUtils::convertKey<PublicKey>(signerKey.key);
            if (PubKeyUtils::hasHint(pubKey, sig.hint) &&
                PubKeyUtils::verifySig(pubKey, sig.signature,
                                       mContentsHash))
            {
                mUsedSignatures[i] = true;
                totalWeight += signerKey.weight;
                if (totalWeight >= neededWeight)
                    return true;

                accountKeyWeights.erase(it);
                break;
            }
        }
    }

    return false;
}

bool
SignatureChecker::checkAllSignaturesUsed() const
{
    for (auto sigb : mUsedSignatures)
    {
        if (!sigb)
        {
            return false;
        }
    }
    return true;
}

const UsedOneTimeSignerKeys &
SignatureChecker::usedOneTimeSignerKeys() const
{
    return mUsedOneTimeSignerKeys;
}

};
