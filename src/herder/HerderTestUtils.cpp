// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderTestUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "herder/LedgerCloseData.h"
#include "xdr/Stellar-ledger.h"

#include <xdrpp/marshal.h>

namespace stellar
{
namespace HerderTestUtils
{

PublicKey
makePublicKey(int i)
{
    auto hash = sha256("NODE_SEED_" + std::to_string(i));
    auto secretKey = SecretKey::fromSeed(hash);
    return secretKey.getPublicKey();
};

SCPQuorumSet
makeSaneQuorumSet()
{
    auto result = SCPQuorumSet{};
    result.threshold = 1;
    result.validators.push_back(makePublicKey(0));
    return result;
}

SCPQuorumSet
makeBigQuorumSet()
{
    auto keys = std::vector<PublicKey>{};
    for (auto i = 0; i < 1001; i++)
    {
        keys.push_back(makePublicKey(i));
    }

    auto bigQSet = SCPQuorumSet{};
    bigQSet.threshold = 1;
    bigQSet.validators.push_back(keys[0]);
    for (auto i = 0; i < 10; i++)
    {
        bigQSet.innerSets.push_back({});
        bigQSet.innerSets.back().threshold = 1;
        for (auto j = i * 100 + 1; j <= (i + 1) * 100; j++)
            bigQSet.innerSets.back().validators.push_back(keys[j]);
    }
    return bigQSet;
}

SCPEnvelope
makeEnvelope(Hash txHash, Hash qSetHash, uint64_t slotIndex)
{
    auto envelope = SCPEnvelope{};
    envelope.statement.slotIndex = slotIndex;
    envelope.statement.pledges.type(SCP_ST_PREPARE);
    envelope.statement.pledges.prepare().ballot.value =
        xdr::xdr_to_opaque(StellarValue{txHash, 10, emptyUpgradeSteps, 0});
    envelope.statement.pledges.prepare().quorumSetHash = qSetHash;
    return envelope;
};
}
}
