// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "history/InferredQuorum.h"
#include "lib/catch.hpp"
#include "main/Config.h"
#include "test/test.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include <xdrpp/autocheck.h>

using namespace stellar;

TEST_CASE("InferredQuorum intersection", "[history][inferredquorum]")
{
    InferredQuorum iq;

    SecretKey skA = SecretKey::random();
    SecretKey skB = SecretKey::random();
    SecretKey skC = SecretKey::random();
    SecretKey skD = SecretKey::random();

    PublicKey pkA = skA.getPublicKey();
    PublicKey pkB = skB.getPublicKey();
    PublicKey pkC = skC.getPublicKey();
    PublicKey pkD = skD.getPublicKey();

    xdr::xvector<SCPQuorumSet> emptySet;
    SCPQuorumSet qsA(2, xdr::xvector<PublicKey>({pkB, pkC, pkD}), emptySet);
    SCPQuorumSet qsB(2, xdr::xvector<PublicKey>({pkA, pkC, pkD}), emptySet);
    SCPQuorumSet qsC(2, xdr::xvector<PublicKey>({pkA, pkB, pkD}), emptySet);
    SCPQuorumSet qsD(2, xdr::xvector<PublicKey>({pkA, pkB, pkC}), emptySet);

    Hash qshA = sha256(xdr::xdr_to_opaque(qsA));
    Hash qshB = sha256(xdr::xdr_to_opaque(qsB));
    Hash qshC = sha256(xdr::xdr_to_opaque(qsC));
    Hash qshD = sha256(xdr::xdr_to_opaque(qsD));

    iq.mPubKeys[pkA]++;
    iq.mPubKeys[pkB]++;
    iq.mPubKeys[pkC]++;
    iq.mPubKeys[pkD]++;

    iq.mQsetHashes.insert(std::make_pair(pkA, qshA));
    iq.mQsetHashes.insert(std::make_pair(pkB, qshB));
    iq.mQsetHashes.insert(std::make_pair(pkC, qshC));
    iq.mQsetHashes.insert(std::make_pair(pkD, qshD));

    iq.mQsets[qshA] = qsA;
    iq.mQsets[qshB] = qsB;
    iq.mQsets[qshC] = qsC;
    iq.mQsets[qshD] = qsD;

    Config cfg(getTestConfig(0, Config::TESTDB_IN_MEMORY_SQLITE));
    CHECK(iq.checkQuorumIntersection(cfg));
}

TEST_CASE("InferredQuorum intersection w subquorums",
          "[history][inferredquorum][subquorum]")
{
    InferredQuorum iq;

    SecretKey skA = SecretKey::random();
    SecretKey skB = SecretKey::random();
    SecretKey skC = SecretKey::random();
    SecretKey skD = SecretKey::random();
    SecretKey skE = SecretKey::random();
    SecretKey skF = SecretKey::random();

    PublicKey pkA = skA.getPublicKey();
    PublicKey pkB = skB.getPublicKey();
    PublicKey pkC = skC.getPublicKey();
    PublicKey pkD = skD.getPublicKey();
    PublicKey pkE = skE.getPublicKey();
    PublicKey pkF = skF.getPublicKey();

    xdr::xvector<PublicKey> noKeys;
    xdr::xvector<SCPQuorumSet> emptySet;

    SCPQuorumSet qsABC(2, xdr::xvector<PublicKey>({pkA, pkB, pkC}), emptySet);
    SCPQuorumSet qsABD(2, xdr::xvector<PublicKey>({pkA, pkB, pkD}), emptySet);
    SCPQuorumSet qsABE(2, xdr::xvector<PublicKey>({pkA, pkB, pkE}), emptySet);
    SCPQuorumSet qsABF(2, xdr::xvector<PublicKey>({pkA, pkB, pkF}), emptySet);

    SCPQuorumSet qsACD(2, xdr::xvector<PublicKey>({pkA, pkC, pkD}), emptySet);
    SCPQuorumSet qsACE(2, xdr::xvector<PublicKey>({pkA, pkC, pkE}), emptySet);
    SCPQuorumSet qsACF(2, xdr::xvector<PublicKey>({pkA, pkC, pkF}), emptySet);

    SCPQuorumSet qsADE(2, xdr::xvector<PublicKey>({pkA, pkD, pkE}), emptySet);
    SCPQuorumSet qsADF(2, xdr::xvector<PublicKey>({pkA, pkD, pkF}), emptySet);

    SCPQuorumSet qsBDC(2, xdr::xvector<PublicKey>({pkB, pkD, pkC}), emptySet);
    SCPQuorumSet qsBDE(2, xdr::xvector<PublicKey>({pkB, pkD, pkE}), emptySet);
    SCPQuorumSet qsCDE(2, xdr::xvector<PublicKey>({pkC, pkD, pkE}), emptySet);

    SCPQuorumSet qsA(2, noKeys,
                     xdr::xvector<SCPQuorumSet>({qsBDC, qsBDE, qsCDE}));
    SCPQuorumSet qsB(2, noKeys,
                     xdr::xvector<SCPQuorumSet>({qsACD, qsACE, qsACF}));
    SCPQuorumSet qsC(2, noKeys,
                     xdr::xvector<SCPQuorumSet>({qsABD, qsABE, qsABF}));

    SCPQuorumSet qsD(2, noKeys,
                     xdr::xvector<SCPQuorumSet>({qsABC, qsABE, qsABF}));
    SCPQuorumSet qsE(2, noKeys,
                     xdr::xvector<SCPQuorumSet>({qsABC, qsABD, qsABF}));
    SCPQuorumSet qsF(2, noKeys,
                     xdr::xvector<SCPQuorumSet>({qsABC, qsABD, qsABE}));

    Hash qshA = sha256(xdr::xdr_to_opaque(qsA));
    Hash qshB = sha256(xdr::xdr_to_opaque(qsB));
    Hash qshC = sha256(xdr::xdr_to_opaque(qsC));
    Hash qshD = sha256(xdr::xdr_to_opaque(qsD));
    Hash qshE = sha256(xdr::xdr_to_opaque(qsE));
    Hash qshF = sha256(xdr::xdr_to_opaque(qsF));

    iq.mPubKeys[pkA]++;
    iq.mPubKeys[pkB]++;
    iq.mPubKeys[pkC]++;
    iq.mPubKeys[pkD]++;
    iq.mPubKeys[pkE]++;
    iq.mPubKeys[pkF]++;

    iq.mQsetHashes.insert(std::make_pair(pkA, qshA));
    iq.mQsetHashes.insert(std::make_pair(pkB, qshB));
    iq.mQsetHashes.insert(std::make_pair(pkC, qshC));
    iq.mQsetHashes.insert(std::make_pair(pkD, qshD));
    iq.mQsetHashes.insert(std::make_pair(pkE, qshE));
    iq.mQsetHashes.insert(std::make_pair(pkF, qshF));

    iq.mQsets[qshA] = qsA;
    iq.mQsets[qshB] = qsB;
    iq.mQsets[qshC] = qsC;
    iq.mQsets[qshD] = qsD;
    iq.mQsets[qshE] = qsE;
    iq.mQsets[qshF] = qsF;

    Config cfg(getTestConfig(0, Config::TESTDB_IN_MEMORY_SQLITE));
    CHECK(iq.checkQuorumIntersection(cfg));
}

TEST_CASE("InferredQuorum non intersection", "[history][inferredquorum]")
{
    InferredQuorum iq;

    SecretKey skA = SecretKey::random();
    SecretKey skB = SecretKey::random();
    SecretKey skC = SecretKey::random();
    SecretKey skD = SecretKey::random();
    SecretKey skE = SecretKey::random();
    SecretKey skF = SecretKey::random();

    PublicKey pkA = skA.getPublicKey();
    PublicKey pkB = skB.getPublicKey();
    PublicKey pkC = skC.getPublicKey();
    PublicKey pkD = skD.getPublicKey();
    PublicKey pkE = skE.getPublicKey();
    PublicKey pkF = skF.getPublicKey();

    xdr::xvector<SCPQuorumSet> emptySet;
    SCPQuorumSet qsA(2, xdr::xvector<PublicKey>({pkB, pkC, pkD, pkE, pkF}),
                     emptySet);
    SCPQuorumSet qsB(2, xdr::xvector<PublicKey>({pkA, pkC, pkD, pkE, pkF}),
                     emptySet);
    SCPQuorumSet qsC(2, xdr::xvector<PublicKey>({pkA, pkB, pkD, pkE, pkF}),
                     emptySet);
    SCPQuorumSet qsD(2, xdr::xvector<PublicKey>({pkA, pkB, pkC, pkE, pkF}),
                     emptySet);
    SCPQuorumSet qsE(2, xdr::xvector<PublicKey>({pkA, pkB, pkC, pkD, pkF}),
                     emptySet);
    SCPQuorumSet qsF(2, xdr::xvector<PublicKey>({pkA, pkB, pkC, pkD, pkE}),
                     emptySet);

    Hash qshA = sha256(xdr::xdr_to_opaque(qsA));
    Hash qshB = sha256(xdr::xdr_to_opaque(qsB));
    Hash qshC = sha256(xdr::xdr_to_opaque(qsC));
    Hash qshD = sha256(xdr::xdr_to_opaque(qsD));
    Hash qshE = sha256(xdr::xdr_to_opaque(qsE));
    Hash qshF = sha256(xdr::xdr_to_opaque(qsF));

    iq.mPubKeys[pkA]++;
    iq.mPubKeys[pkB]++;
    iq.mPubKeys[pkC]++;
    iq.mPubKeys[pkD]++;
    iq.mPubKeys[pkE]++;
    iq.mPubKeys[pkF]++;

    iq.mQsetHashes.insert(std::make_pair(pkA, qshA));
    iq.mQsetHashes.insert(std::make_pair(pkB, qshB));
    iq.mQsetHashes.insert(std::make_pair(pkC, qshC));
    iq.mQsetHashes.insert(std::make_pair(pkD, qshD));
    iq.mQsetHashes.insert(std::make_pair(pkE, qshE));
    iq.mQsetHashes.insert(std::make_pair(pkF, qshF));

    iq.mQsets[qshA] = qsA;
    iq.mQsets[qshB] = qsB;
    iq.mQsets[qshC] = qsC;
    iq.mQsets[qshD] = qsD;
    iq.mQsets[qshE] = qsE;
    iq.mQsets[qshF] = qsF;

    Config cfg(getTestConfig(0, Config::TESTDB_IN_MEMORY_SQLITE));
    CHECK(!iq.checkQuorumIntersection(cfg));
}

TEST_CASE("InferredQuorum non intersection w subquorums",
          "[history][inferredquorum][subquorum]")
{
    InferredQuorum iq;

    SecretKey skA = SecretKey::random();
    SecretKey skB = SecretKey::random();
    SecretKey skC = SecretKey::random();
    SecretKey skD = SecretKey::random();
    SecretKey skE = SecretKey::random();
    SecretKey skF = SecretKey::random();

    PublicKey pkA = skA.getPublicKey();
    PublicKey pkB = skB.getPublicKey();
    PublicKey pkC = skC.getPublicKey();
    PublicKey pkD = skD.getPublicKey();
    PublicKey pkE = skE.getPublicKey();
    PublicKey pkF = skF.getPublicKey();

    xdr::xvector<PublicKey> noKeys;
    xdr::xvector<SCPQuorumSet> emptySet;

    SCPQuorumSet qsABC(2, xdr::xvector<PublicKey>({pkA, pkB, pkC}), emptySet);
    SCPQuorumSet qsABD(2, xdr::xvector<PublicKey>({pkA, pkB, pkD}), emptySet);
    SCPQuorumSet qsABE(2, xdr::xvector<PublicKey>({pkA, pkB, pkE}), emptySet);
    SCPQuorumSet qsABF(2, xdr::xvector<PublicKey>({pkA, pkB, pkF}), emptySet);

    SCPQuorumSet qsACD(2, xdr::xvector<PublicKey>({pkA, pkC, pkD}), emptySet);
    SCPQuorumSet qsACE(2, xdr::xvector<PublicKey>({pkA, pkC, pkE}), emptySet);
    SCPQuorumSet qsACF(2, xdr::xvector<PublicKey>({pkA, pkC, pkF}), emptySet);

    SCPQuorumSet qsADE(2, xdr::xvector<PublicKey>({pkA, pkD, pkE}), emptySet);
    SCPQuorumSet qsADF(2, xdr::xvector<PublicKey>({pkA, pkD, pkF}), emptySet);

    SCPQuorumSet qsBDC(2, xdr::xvector<PublicKey>({pkB, pkD, pkC}), emptySet);
    SCPQuorumSet qsBDE(2, xdr::xvector<PublicKey>({pkB, pkD, pkE}), emptySet);
    SCPQuorumSet qsBDF(2, xdr::xvector<PublicKey>({pkB, pkD, pkF}), emptySet);
    SCPQuorumSet qsCDE(2, xdr::xvector<PublicKey>({pkC, pkD, pkE}), emptySet);
    SCPQuorumSet qsCDF(2, xdr::xvector<PublicKey>({pkC, pkD, pkF}), emptySet);

    SCPQuorumSet qsA(2, noKeys,
                     xdr::xvector<SCPQuorumSet>({qsABC, qsABD, qsABE}));
    SCPQuorumSet qsB(2, noKeys,
                     xdr::xvector<SCPQuorumSet>({qsBDC, qsABD, qsABF}));
    SCPQuorumSet qsC(2, noKeys,
                     xdr::xvector<SCPQuorumSet>({qsACD, qsACD, qsACF}));

    SCPQuorumSet qsD(2, noKeys,
                     xdr::xvector<SCPQuorumSet>({qsCDE, qsADE, qsBDE}));
    SCPQuorumSet qsE(2, noKeys,
                     xdr::xvector<SCPQuorumSet>({qsCDE, qsADE, qsBDE}));
    SCPQuorumSet qsF(2, noKeys,
                     xdr::xvector<SCPQuorumSet>({qsABF, qsADF, qsBDF}));

    Hash qshA = sha256(xdr::xdr_to_opaque(qsA));
    Hash qshB = sha256(xdr::xdr_to_opaque(qsB));
    Hash qshC = sha256(xdr::xdr_to_opaque(qsC));
    Hash qshD = sha256(xdr::xdr_to_opaque(qsD));
    Hash qshE = sha256(xdr::xdr_to_opaque(qsE));
    Hash qshF = sha256(xdr::xdr_to_opaque(qsF));

    iq.mPubKeys[pkA]++;
    iq.mPubKeys[pkB]++;
    iq.mPubKeys[pkC]++;
    iq.mPubKeys[pkD]++;
    iq.mPubKeys[pkE]++;
    iq.mPubKeys[pkF]++;

    iq.mQsetHashes.insert(std::make_pair(pkA, qshA));
    iq.mQsetHashes.insert(std::make_pair(pkB, qshB));
    iq.mQsetHashes.insert(std::make_pair(pkC, qshC));
    iq.mQsetHashes.insert(std::make_pair(pkD, qshD));
    iq.mQsetHashes.insert(std::make_pair(pkE, qshE));
    iq.mQsetHashes.insert(std::make_pair(pkF, qshF));

    iq.mQsets[qshA] = qsA;
    iq.mQsets[qshB] = qsB;
    iq.mQsets[qshC] = qsC;
    iq.mQsets[qshD] = qsD;
    iq.mQsets[qshE] = qsE;
    iq.mQsets[qshF] = qsF;

    Config cfg(getTestConfig(0, Config::TESTDB_IN_MEMORY_SQLITE));
    CHECK(!iq.checkQuorumIntersection(cfg));
}
