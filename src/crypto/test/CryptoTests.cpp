// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/BLAKE2.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "crypto/ShortHash.h"
#include "crypto/SignerKey.h"
#include "crypto/StrKey.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "test/test.h"
#include "util/Logging.h"
#include "xdr/Stellar-types.h"
#include <autocheck/autocheck.hpp>
#include <map>
#include <regex>
#include <sodium.h>
#include <stdexcept>

using namespace stellar;

static std::map<std::vector<uint8_t>, std::string> hexTestVectors = {
    {{}, ""},
    {{0x72}, "72"},
    {{0x54, 0x4c}, "544c"},
    {{0x34, 0x75, 0x52, 0x45, 0x34, 0x75}, "347552453475"},
    {{0x4f, 0x46, 0x79, 0x58, 0x43, 0x6d, 0x68, 0x37, 0x51},
     "4f467958436d683751"}};

TEST_CASE("random", "[crypto]")
{
    SecretKey k1 = SecretKey::pseudoRandomForTesting();
    SecretKey k2 = SecretKey::pseudoRandomForTesting();
    LOG_DEBUG(DEFAULT_LOG, "k1: {}", k1.getStrKeySeed().value);
    LOG_DEBUG(DEFAULT_LOG, "k2: {}", k2.getStrKeySeed().value);
    CHECK(k1.getStrKeySeed() != k2.getStrKeySeed());

    SecretKey k1b = SecretKey::fromStrKeySeed(k1.getStrKeySeed().value);
    REQUIRE(k1 == k1b);
    REQUIRE(k1.getPublicKey() == k1b.getPublicKey());
}

TEST_CASE("hex tests", "[crypto]")
{
    // Do some fixed test vectors.
    for (auto const& pair : hexTestVectors)
    {
        LOG_DEBUG(DEFAULT_LOG, "fixed test vector hex: \"{}\"", pair.second);

        auto enc = binToHex(pair.first);
        CHECK(enc.size() == pair.second.size());
        CHECK(enc == pair.second);

        auto dec = hexToBin(pair.second);
        CHECK(pair.first == dec);
    }

    // Do 20 random round-trip tests.
    autocheck::check<std::vector<uint8_t>>(
        [](std::vector<uint8_t> v) {
            auto enc = binToHex(v);
            auto dec = hexToBin(enc);
            LOG_DEBUG(DEFAULT_LOG, "random round-trip hex: \"{}\"", enc);
            CHECK(v == dec);
            return v == dec;
        },
        20);
}

static std::map<std::string, std::string> sha256TestVectors = {
    {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},

    {"a", "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"},

    {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},

    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}};

TEST_CASE("SHA256 tests", "[crypto]")
{
    // Do some fixed test vectors.
    for (auto const& pair : sha256TestVectors)
    {
        LOG_DEBUG(DEFAULT_LOG, "fixed test vector SHA256: \"{}\"", pair.second);

        auto hash = binToHex(sha256(pair.first));
        CHECK(hash.size() == pair.second.size());
        CHECK(hash == pair.second);
    }
}

TEST_CASE("Stateful SHA256 tests", "[crypto]")
{
    // Do some fixed test vectors.
    for (auto const& pair : sha256TestVectors)
    {
        LOG_DEBUG(DEFAULT_LOG, "fixed test vector SHA256: \"{}\"", pair.second);
        SHA256 h;
        h.add(pair.first);
        auto hash = binToHex(h.finish());
        CHECK(hash.size() == pair.second.size());
        CHECK(hash == pair.second);
    }
}

TEST_CASE("XDRSHA256 is identical to byte SHA256", "[crypto]")
{
    for (size_t i = 0; i < 1000; ++i)
    {
        auto entry = LedgerTestUtils::generateValidLedgerEntry(100);
        auto bytes_hash = sha256(xdr::xdr_to_opaque(entry));
        auto stream_hash = xdrSha256(entry);
        CHECK(bytes_hash == stream_hash);
    }
}

TEST_CASE("SHA256 bytes bench", "[!hide][sha-bytes-bench]")
{
    shortHash::initialize();
    autocheck::rng().seed(11111);
    std::vector<LedgerEntry> entries;
    for (size_t i = 0; i < 1000; ++i)
    {
        entries.emplace_back(LedgerTestUtils::generateValidLedgerEntry(1000));
    }
    for (size_t i = 0; i < 10000; ++i)
    {
        for (auto const& e : entries)
        {
            auto opaque = xdr::xdr_to_opaque(e);
            sha256(opaque);
        }
    }
}

TEST_CASE("SHA256 XDR bench", "[!hide][sha-xdr-bench]")
{
    shortHash::initialize();
    autocheck::rng().seed(11111);
    std::vector<LedgerEntry> entries;
    for (size_t i = 0; i < 1000; ++i)
    {
        entries.emplace_back(LedgerTestUtils::generateValidLedgerEntry(1000));
    }
    for (size_t i = 0; i < 10000; ++i)
    {
        for (auto const& e : entries)
        {
            xdrSha256(e);
        }
    }
}

static std::map<std::string, std::string> blake2TestVectors = {
    {"", "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8"},

    {"a", "8928aae63c84d87ea098564d1e03ad813f107add474e56aedd286349c0c03ea4"},

    {"abc", "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319"},

    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "5f7a93da9c5621583f22e49e8e91a40cbba37536622235a380f434b9f68e49c4"}};

TEST_CASE("BLAKE2 tests", "[crypto]")
{
    // Do some fixed test vectors.
    for (auto const& pair : blake2TestVectors)
    {
        LOG_DEBUG(DEFAULT_LOG, "fixed test vector BLAKE2: \"{}\"", pair.second);

        auto hash = binToHex(blake2(pair.first));
        CHECK(hash.size() == pair.second.size());
        CHECK(hash == pair.second);
    }
}

TEST_CASE("Stateful BLAKE2 tests", "[crypto]")
{
    // Do some fixed test vectors.
    for (auto const& pair : blake2TestVectors)
    {
        LOG_DEBUG(DEFAULT_LOG, "fixed test vector BLAKE2: \"{}\"", pair.second);
        BLAKE2 h;
        h.add(pair.first);
        auto hash = binToHex(h.finish());
        CHECK(hash.size() == pair.second.size());
        CHECK(hash == pair.second);
    }
}

TEST_CASE("XDRBLAKE2 is identical to byte BLAKE2", "[crypto]")
{
    for (size_t i = 0; i < 1000; ++i)
    {
        auto entry = LedgerTestUtils::generateValidLedgerEntry(100);
        auto bytes_hash = blake2(xdr::xdr_to_opaque(entry));
        auto stream_hash = xdrBlake2(entry);
        CHECK(bytes_hash == stream_hash);
    }
}

TEST_CASE("BLAKE2 bytes bench", "[!hide][blake-bytes-bench]")
{
    shortHash::initialize();
    autocheck::rng().seed(11111);
    std::vector<LedgerEntry> entries;
    for (size_t i = 0; i < 1000; ++i)
    {
        entries.emplace_back(LedgerTestUtils::generateValidLedgerEntry(1000));
    }
    for (size_t i = 0; i < 10000; ++i)
    {
        for (auto const& e : entries)
        {
            auto opaque = xdr::xdr_to_opaque(e);
            blake2(opaque);
        }
    }
}

TEST_CASE("BLAKE2 XDR bench", "[!hide][blake-xdr-bench]")
{
    shortHash::initialize();
    autocheck::rng().seed(11111);
    std::vector<LedgerEntry> entries;
    for (size_t i = 0; i < 1000; ++i)
    {
        entries.emplace_back(LedgerTestUtils::generateValidLedgerEntry(1000));
    }
    for (size_t i = 0; i < 10000; ++i)
    {
        for (auto const& e : entries)
        {
            xdrBlake2(e);
        }
    }
}

TEST_CASE("HMAC test vector", "[crypto]")
{
    HmacSha256Key k;
    k.key[0] = 'k';
    k.key[1] = 'e';
    k.key[2] = 'y';
    auto s = "The quick brown fox jumps over the lazy dog";
    auto h = hexToBin256(
        "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    auto v = hmacSha256(k, s);
    REQUIRE(h == v.mac);
    REQUIRE(hmacSha256Verify(v, k, s));
}

TEST_CASE("HKDF test vector", "[crypto]")
{
    auto ikm = hexToBin("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    HmacSha256Key prk, okm;
    prk.key = hexToBin256(
        "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04");
    okm.key = hexToBin256(
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d");
    REQUIRE(hkdfExtract(ikm) == prk);
    std::vector<uint8_t> empty;
    REQUIRE(hkdfExpand(prk, empty) == okm);
}

TEST_CASE("sign tests", "[crypto]")
{
    auto sk = SecretKey::pseudoRandomForTesting();
    auto pk = sk.getPublicKey();
    LOG_DEBUG(DEFAULT_LOG, "generated random secret key seed: {}",
              sk.getStrKeySeed().value);
    LOG_DEBUG(DEFAULT_LOG, "corresponding public key: {}",
              KeyUtils::toStrKey(pk));

    CHECK(SecretKey::fromStrKeySeed(sk.getStrKeySeed().value) == sk);

    std::string msg = "hello";
    auto sig = sk.sign(msg);

    LOG_DEBUG(DEFAULT_LOG, "formed signature: {}", binToHex(sig));

    LOG_DEBUG(DEFAULT_LOG, "checking signature-verify");
    CHECK(PubKeyUtils::verifySig(pk, sig, msg));

    LOG_DEBUG(DEFAULT_LOG, "checking verify-failure on bad message");
    CHECK(!PubKeyUtils::verifySig(pk, sig, std::string("helloo")));

    LOG_DEBUG(DEFAULT_LOG, "checking verify-failure on bad signature");
    sig[4] ^= 1;
    CHECK(!PubKeyUtils::verifySig(pk, sig, msg));
}

TEST_CASE("sign and verify benchmarking", "[crypto-bench][bench][!hide]")
{
    size_t signPerSec = 0, verifyPerSec = 0;
    LOG_INFO(DEFAULT_LOG, "Benchmarking signatures and verifications");
    SecretKey::benchmarkOpsPerSecond(signPerSec, verifyPerSec, 10000);
    LOG_INFO(DEFAULT_LOG, "Benchmarked {} signatures / sec", signPerSec);
    LOG_INFO(DEFAULT_LOG, "Benchmarked {} verifications / sec", verifyPerSec);
}

TEST_CASE("verify-hit benchmarking", "[crypto-bench][bench][!hide]")
{
    size_t signPerSec = 0, verifyPerSec = 0;
    LOG_INFO(DEFAULT_LOG, "Benchmarking signatures and verify cache-hits");
    SecretKey::benchmarkOpsPerSecond(signPerSec, verifyPerSec, 10000, 10);
    LOG_INFO(DEFAULT_LOG, "Benchmarked {} signatures / sec", signPerSec);
    LOG_INFO(DEFAULT_LOG, "Benchmarked {} verification cache-hits / sec",
             verifyPerSec);
}

TEST_CASE("StrKey tests", "[crypto]")
{
    std::regex b32("^([A-Z2-7])+$");
    std::regex b32Pad("^([A-Z2-7])+(=|===|====|======)?$");

    autocheck::generator<std::vector<uint8_t>> input;

    auto randomB32 = []() {
        char res;
        char d = static_cast<char>(gRandomEngine() % 32);
        if (d < 6)
        {
            res = d + '2';
        }
        else
        {
            res = d - 6 + 'A';
        }
        return res;
    };

    uint8_t version = 2;

    // check round trip
    for (size_t size = 0; size < 100; size++)
    {
        std::vector<uint8_t> in(input(size));

        std::string encoded = strKey::toStrKey(version, in).value;

        REQUIRE(encoded.size() == ((size + 3 + 4) / 5 * 8));

        // check the no padding case
        if ((size + 3) % 5 == 0)
        {
            REQUIRE(std::regex_match(encoded, b32));
        }
        else
        {
            REQUIRE(std::regex_match(encoded, b32Pad));
        }

        uint8_t decodedVer = 0;
        std::vector<uint8_t> decoded;
        REQUIRE(strKey::fromStrKey(encoded, decodedVer, decoded));

        REQUIRE(decodedVer == version);
        REQUIRE(decoded == in);
    }

    // basic corruption check on a fixed size
    size_t n_corrupted = 0;
    size_t n_detected = 0;

    for (int round = 0; round < 100; round++)
    {
        const int expectedSize = 32;
        std::vector<uint8_t> in(input(expectedSize));
        std::string encoded = strKey::toStrKey(version, in).value;

        for (size_t p = 0u; p < encoded.size(); p++)
        {
            // perform a single corruption
            for (int st = 0; st < 4; st++)
            {
                std::string corrupted(encoded);
                auto pos = corrupted.begin() + p;
                switch (st)
                {
                case 0:
                    // remove
                    corrupted.erase(pos);
                    break;
                case 1:
                    // modify
                    corrupted[p] = randomB32();
                    break;
                case 2:
                    // duplicate element
                    corrupted.insert(pos, corrupted[p]);
                    break;
                case 3:
                    // swap consecutive elements
                    if (p > 0)
                    {
                        std::swap(corrupted[p], corrupted[p - 1]);
                    }
                    break;
                default:
                    abort();
                }
                uint8_t ver;
                std::vector<uint8_t> dt;
                if (corrupted != encoded)
                {
                    bool sameSize = (corrupted.size() == encoded.size());
                    if (sameSize)
                    {
                        n_corrupted++;
                    }
                    bool res = !strKey::fromStrKey(corrupted, ver, dt);
                    if (res)
                    {
                        if (sameSize)
                        {
                            ++n_detected;
                        }
                    }
                    else
                    {
                        LOG_WARNING(DEFAULT_LOG,
                                    "Failed to detect strkey corruption");
                        LOG_WARNING(DEFAULT_LOG, " original: {}", encoded);
                        LOG_WARNING(DEFAULT_LOG, "  corrupt: {}", corrupted);
                    }
                    if (!sameSize)
                    {
                        // extra/missing data must be detected
                        REQUIRE(res);
                    }
                }
            }
        }
    }

    // CCITT CRC16 theoretical maximum "uncorrelated error" detection rate
    // is 99.9984% (1 undetected failure in 2^16); but we're not running an
    // infinite (or even 2^16) sized set of inputs and our mutations are
    // highly structured, so we give it some leeway.
    // To give us good odds of making it through integration tests
    // we set the threshold quite wide here, to 99.99%. The test is very
    // slightly nondeterministic but this should give it plenty of leeway.

    double detectionRate =
        (((double)n_detected) / ((double)n_corrupted)) * 100.0;
    LOG_INFO(DEFAULT_LOG, "CRC16 error-detection rate {}", detectionRate);
    REQUIRE(detectionRate > 99.99);
}

TEST_CASE("key string roundtrip", "[crypto]")
{
    SignerKey signer;
    auto publicKey = SecretKey::pseudoRandomForTesting().getPublicKey();
    uint256 rand256 = publicKey.ed25519();
    SECTION("SIGNER_KEY_TYPE_ED25519")
    {
        signer.type(SIGNER_KEY_TYPE_ED25519);
        signer.ed25519() = rand256;
        REQUIRE(KeyUtils::fromStrKey<SignerKey>(KeyUtils::toStrKey(signer)) ==
                signer);
    }
    SECTION("SIGNER_KEY_TYPE_PRE_AUTH_TX")
    {
        signer.type(SIGNER_KEY_TYPE_PRE_AUTH_TX);
        signer.preAuthTx() = rand256;
        REQUIRE(KeyUtils::fromStrKey<SignerKey>(KeyUtils::toStrKey(signer)) ==
                signer);
    }
    SECTION("SIGNER_KEY_TYPE_HASH_X")
    {
        signer.type(SIGNER_KEY_TYPE_HASH_X);
        signer.hashX() = rand256;
        REQUIRE(KeyUtils::fromStrKey<SignerKey>(KeyUtils::toStrKey(signer)) ==
                signer);
    }
    SECTION("SIGNER_KEY_TYPE_ED25519_SIGNED_PAYLOAD")
    {
        signer.type(SIGNER_KEY_TYPE_ED25519_SIGNED_PAYLOAD);
        signer.ed25519SignedPayload().ed25519 = rand256;

        for (uint32_t i = 0;
             i < signer.ed25519SignedPayload().payload.max_size(); ++i)
        {
            signer.ed25519SignedPayload().payload.emplace_back('1');
            REQUIRE(KeyUtils::fromStrKey<SignerKey>(
                        KeyUtils::toStrKey(signer)) == signer);
        }
    }
    SECTION("public key")
    {
        REQUIRE(KeyUtils::fromStrKey<PublicKey>(
                    KeyUtils::toStrKey(publicKey)) == publicKey);
    }
}

// The following test vectors are taken from
// https://eprint.iacr.org/2020/1244.pdf and the Zcash project's ZIP215 work as
// described in https://hdevalence.ca/blog/2020-10-04-its-25519am
//
// They are explained in more detail in the soroban-env-host test file
// ed25519_edge_cases.rs. We run the same vectors here to confirm that libsodium
// and dalek behave the same way on various edge cases.

struct Iacr20201244TestVector
{
    char const* message;
    char const* pub_key;
    char const* signature;
    bool should_fail;
};

const Iacr20201244TestVector IACR_2020_1244_TEST_VECTORS[12] = {
    // Case 0: Small-order A and R components (should be rejected) but verifies
    // under either equality check.
    Iacr20201244TestVector{
        "8c93255d71dcab10e8f379c26200f3c7bd5f09d9bc3068d3ef4edeb4853022b6",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
        true,
    },
    // Case 1: Small-order A component (should be rejected) but verifies under
    // either equality check.
    Iacr20201244TestVector{
        "9bd9f44f4dcc75bd531b56b2cd280b0bb38fc1cd6d1230e14861d861de092e79",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "f7badec5b8abeaf699583992219b7b223f1df3fbbea919844e3f7c554a43dd43a5"
        "bb704786be79fc476f91d3f3f89b03984d8068dcf1bb7dfc6637b45450ac04",
        true,
    },
    // Case 2: Small-order R component (should be rejected) but verifies under
    // either equality check.
    Iacr20201244TestVector{
        "aebf3f2601a0c8c5d39cc7d8911642f740b78168218da8471772b35f9d35b9ab",
        "f7badec5b8abeaf699583992219b7b223f1df3fbbea919844e3f7c554a43dd43",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa8c"
        "4bd45aecaca5b24fb97bc10ac27ac8751a7dfe1baff8b953ec9f5833ca260e",
        true,
    },
    // Case 3: Mixed-order A and R, verifies under either equality check, should
    // be accepted.
    Iacr20201244TestVector{
        "9bd9f44f4dcc75bd531b56b2cd280b0bb38fc1cd6d1230e14861d861de092e79",
        "cdb267ce40c5cd45306fa5d2f29731459387dbf9eb933b7bd5aed9a765b88d4d",
        "9046a64750444938de19f227bb80485e92b83fdb4b6506c160484c016cc1852f87"
        "909e14428a7a1d62e9f22f3d3ad7802db02eb2e688b6c52fcd6648a98bd009",
        false,
    },
    // Case 4: Mixed-order A and R, only verifies under cofactor equality check,
    // should be rejected.
    Iacr20201244TestVector{
        "e47d62c63f830dc7a6851a0b1f33ae4bb2f507fb6cffec4011eaccd55b53f56c",
        "cdb267ce40c5cd45306fa5d2f29731459387dbf9eb933b7bd5aed9a765b88d4d",
        "160a1cb0dc9c0258cd0a7d23e94d8fa878bcb1925f2c64246b2dee1796bed5125e"
        "c6bc982a269b723e0668e540911a9a6a58921d6925e434ab10aa7940551a09",
        true,
    },
    // Case 5: Mixed-order A, order-L R, only verifies under cofactor equality
    // check, should be rejected.
    Iacr20201244TestVector{
        "e47d62c63f830dc7a6851a0b1f33ae4bb2f507fb6cffec4011eaccd55b53f56c",
        "cdb267ce40c5cd45306fa5d2f29731459387dbf9eb933b7bd5aed9a765b88d4d",
        "21122a84e0b5fca4052f5b1235c80a537878b38f3142356b2c2384ebad4668b7e4"
        "0bc836dac0f71076f9abe3a53f9c03c1ceeeddb658d0030494ace586687405",
        true,
    },
    // Case 6: Order-L A and R, non-canonical S (> L), should be rejected.
    Iacr20201244TestVector{
        "85e241a07d148b41e47d62c63f830dc7a6851a0b1f33ae4bb2f507fb6cffec40",
        "442aad9f089ad9e14647b1ef9099a1ff4798d78589e66f28eca69c11f582a623",
        "e96f66be976d82e60150baecff9906684aebb1ef181f67a7189ac78ea23b6c0e54"
        "7f7690a0e2ddcd04d87dbc3490dc19b3b3052f7ff0538cb68afb369ba3a514",
        true,
    },
    // Case 7: Order-L A and R, non-canonical S (>> L) in a way that fails
    // bitwise canonicity tests, should be rejected.
    //
    // NB: There's a typo (an extra 'e') in the middle of test vector 7's
    // signature in the appendix of the paper itself, but this is corrected in
    // Novi's formal / machine-generated testcases in
    // https://github.com/novifinancial/ed25519-speccheck/blob/main/cases.txt
    Iacr20201244TestVector{
        "85e241a07d148b41e47d62c63f830dc7a6851a0b1f33ae4bb2f507fb6cffec40",
        "442aad9f089ad9e14647b1ef9099a1ff4798d78589e66f28eca69c11f582a623",
        "8ce5b96c8f26d0ab6c47958c9e68b937104cd36e13c33566acd2fe8d38aa19427e"
        "71f98a473474f2f13f06f97c20d58cc3f54b8bd0d272f42b695dd7e89a8c22",
        true,
    },
    // Case 8: Non-canonical R, should fail.
    Iacr20201244TestVector{
        "9bedc267423725d473888631ebf45988bad3db83851ee85c85e241a07d148b41",
        "f7badec5b8abeaf699583992219b7b223f1df3fbbea919844e3f7c554a43dd43",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff03"
        "be9678ac102edcd92b0210bb34d7428d12ffc5df5f37e359941266a4e35f0f",
        true},
    // Case 9: Non-canonical R noticed at a different phase of checking in some
    // implementations, should also fail.
    Iacr20201244TestVector{
        "9bedc267423725d473888631ebf45988bad3db83851ee85c85e241a07d148b41",
        "f7badec5b8abeaf699583992219b7b223f1df3fbbea919844e3f7c554a43dd43",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffca"
        "8c5b64cd208982aa38d4936621a4775aa233aa0505711d8fdcfdaa943d4908",
        true},
    // Case 10: Non-canonical A
    Iacr20201244TestVector{
        "e96b7021eb39c1a163b6da4e3093dcd3f21387da4cc4572be588fafae23c155b",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "a9d55260f765261eb9b84e106f665e00b867287a761990d7135963ee0a7d59dca5"
        "bb704786be79fc476f91d3f3f89b03984d8068dcf1bb7dfc6637b45450ac04",
        true},
    // Case 11: Non-canonical A noticed at a different phase of checking in some
    // implementations, should also fail.
    Iacr20201244TestVector{
        "39a591f5321bbe07fd5a23dc2f39d025d74526615746727ceefd6e82ae65c06f",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "a9d55260f765261eb9b84e106f665e00b867287a761990d7135963ee0a7d59dca5"
        "bb704786be79fc476f91d3f3f89b03984d8068dcf1bb7dfc6637b45450ac04",
        true}};

TEST_CASE("Ed25519 test vectors from IACR 2020/1244", "[crypto]")
{
    for (auto const& tv : IACR_2020_1244_TEST_VECTORS)
    {
        PublicKey pk;
        pk.type(PUBLIC_KEY_TYPE_ED25519);
        pk.ed25519() = hexToBin256(tv.pub_key);
        auto s = hexToBin(tv.signature);
        REQUIRE(s.size() == 64);
        Signature sig;
        sig.assign(s.begin(), s.end());
        REQUIRE(PubKeyUtils::verifySig(pk, sig, hexToBin(tv.message)) !=
                tv.should_fail);
    }
}

struct ZcashTestVector
{
    char const* public_key;
    char const* signature;
};

ZcashTestVector const ZCASH_TEST_VECTORS[196] = {
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000000",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000080",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "0100000000000000000000000000000000000000000000000000000000000080",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "000000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc0500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc8500"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "010000000000000000000000000000000000000000000000000000000000008000"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f00"
        "00000000000000000000000000000000000000000000000000000000000000",
    },
    ZcashTestVector{
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00"
        "00000000000000000000000000000000000000000000000000000000000000",
    }};

TEST_CASE("Ed25519 test vectors from Zcash", "[crypto]")
{
    for (auto const& tv : ZCASH_TEST_VECTORS)
    {
        PublicKey pk;
        pk.type(PUBLIC_KEY_TYPE_ED25519);
        pk.ed25519() = hexToBin256(tv.public_key);
        auto s = hexToBin(tv.signature);
        REQUIRE(s.size() == 64);
        Signature sig;
        sig.assign(s.begin(), s.end());
        REQUIRE(!PubKeyUtils::verifySig(pk, sig, std::string("Zcash")));
    }
}
