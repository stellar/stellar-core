// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "crypto/StrKey.h"
#include "lib/catch.hpp"
#include "test/test.h"
#include "util/Logging.h"
#include "util/basen.h"
#include <autocheck/autocheck.hpp>
#include <map>
#include <regex>
#include <sodium.h>

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
    SecretKey k1 = SecretKey::random();
    SecretKey k2 = SecretKey::random();
    LOG(DEBUG) << "k1: " << k1.getStrKeySeed().value;
    LOG(DEBUG) << "k2: " << k2.getStrKeySeed().value;
    CHECK(k1.getStrKeySeed() != k2.getStrKeySeed());
}

TEST_CASE("hex tests", "[crypto]")
{
    // Do some fixed test vectors.
    for (auto const& pair : hexTestVectors)
    {
        LOG(DEBUG) << "fixed test vector hex: \"" << pair.second << "\"";

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
            LOG(DEBUG) << "random round-trip hex: \"" << enc << "\"";
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
        LOG(DEBUG) << "fixed test vector SHA256: \"" << pair.second << "\"";

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
        LOG(DEBUG) << "fixed test vector SHA256: \"" << pair.second << "\"";
        auto h = SHA256::create();
        h->add(pair.first);
        auto hash = binToHex(h->finish());
        CHECK(hash.size() == pair.second.size());
        CHECK(hash == pair.second);
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
    auto sk = SecretKey::random();
    auto pk = sk.getPublicKey();
    LOG(DEBUG) << "generated random secret key seed: "
               << sk.getStrKeySeed().value;
    LOG(DEBUG) << "corresponding public key: " << KeyUtils::toStrKey(pk);

    CHECK(SecretKey::fromStrKeySeed(sk.getStrKeySeed().value) == sk);

    std::string msg = "hello";
    auto sig = sk.sign(msg);

    LOG(DEBUG) << "formed signature: " << binToHex(sig);

    LOG(DEBUG) << "checking signature-verify";
    CHECK(PubKeyUtils::verifySig(pk, sig, msg));

    LOG(DEBUG) << "checking verify-failure on bad message";
    CHECK(!PubKeyUtils::verifySig(pk, sig, std::string("helloo")));

    LOG(DEBUG) << "checking verify-failure on bad signature";
    sig[4] ^= 1;
    CHECK(!PubKeyUtils::verifySig(pk, sig, msg));
}

struct SignVerifyTestcase
{
    SecretKey key;
    PublicKey pub;
    std::vector<uint8_t> msg;
    Signature sig;
    void
    sign()
    {
        sig = key.sign(msg);
    }
    void
    verify()
    {
        CHECK(PubKeyUtils::verifySig(pub, sig, msg));
    }
    static SignVerifyTestcase
    create()
    {
        SignVerifyTestcase st;
        st.key = SecretKey::random();
        st.pub = st.key.getPublicKey();
        st.msg = randomBytes(256);
        return st;
    }
};

TEST_CASE("sign and verify benchmarking", "[crypto-bench][bench][hide]")
{
    size_t n = 100000;
    std::vector<SignVerifyTestcase> cases;
    for (size_t i = 0; i < n; ++i)
    {
        cases.push_back(SignVerifyTestcase::create());
    }

    LOG(INFO) << "Benchmarking " << n << " signatures and verifications";
    {
        TIMED_SCOPE(timerBlkObj, "signing");
        for (auto& c : cases)
        {
            c.sign();
        }
    }

    {
        TIMED_SCOPE(timerBlkObj, "verifying");
        for (auto& c : cases)
        {
            c.verify();
        }
    }
}

TEST_CASE("StrKey tests", "[crypto]")
{
    std::regex b32("^([A-Z2-7])+$");
    std::regex b32Pad("^([A-Z2-7])+(=|===|====|======)?$");

    autocheck::generator<std::vector<uint8_t>> input;

    uint8_t version = 2;

    // check round trip
    for (int size = 0; size < 100; size++)
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

    for (int round = 0; round < 5; round++)
    {
        const int expectedSize = 32;
        std::vector<uint8_t> in(input(expectedSize));
        std::string encoded = strKey::toStrKey(version, in).value;

        for (size_t p = 0u; p < encoded.size(); p++)
        {
            for (int st = 0; st < 4; st++)
            {
                std::string corrupted(encoded);
                auto pos = corrupted.begin() + p;
                switch (st)
                {
                case 0:
                    if (corrupted[p] == 'A' && p + 1 == encoded.size())
                    {
                        // trailing 'A' is equivalent to 0 (and can be dropped)
                        continue;
                    }
                    else
                    {
                        corrupted.erase(pos);
                        break;
                    }
                case 1:
                    corrupted[p]++;
                    break;
                case 2:
                    corrupted.insert(pos, corrupted[p]);
                    break;
                default:
                    if (p > 0 && corrupted[p] != corrupted[p - 1])
                    {
                        std::swap(corrupted[p], corrupted[p - 1]);
                    }
                    else
                    {
                        continue;
                    }
                }
                uint8_t ver;
                std::vector<uint8_t> dt;
                if (corrupted != encoded)
                {
                    n_corrupted++;
                    bool res = !strKey::fromStrKey(corrupted, ver, dt);
                    if (res)
                    {
                        ++n_detected;
                    }
                    else
                    {
                        LOG(WARNING) << "Failed to detect strkey corruption";
                        LOG(WARNING) << " original: " << encoded;
                        LOG(WARNING) << "  corrupt: " << corrupted;
                    }
                }
            }
        }
    }

    // CCITT CRC16 theoretical maximum "uncorrelated error" detection rate
    // is 99.9984% (1 undetected failure in 2^16); but we're not running an
    // infinite (or even 2^16) sized set of inputs and our mutations are
    // highly structured, so we give it some leeway. This is arbitrary but
    // from watching the test above we seem to only get one undetected
    // corruption pair in maybe 50 runs failing, each run being about 1000
    // cases. To give us good odds of making it through integration tests
    // we set the threshold quite wide here, to 98%. The test is very
    // slighly nondeterministic but this should give it plenty of leeway.

    double detectionRate =
        (((double)n_detected) / ((double)n_corrupted)) * 100.0;
    LOG(INFO) << "CRC16 error-detection rate " << detectionRate;
    REQUIRE(detectionRate > 98.0);
}

TEST_CASE("base64 tests", "[crypto]")
{
    autocheck::generator<std::vector<uint8_t>> input;
    // check round trip
    for (int s = 0; s < 100; s++)
    {
        std::vector<uint8_t> in(input(s));

        std::string encoded = bn::encode_b64(in);

        std::vector<uint8_t> decoded;

        bn::decode_b64(encoded, decoded);

        REQUIRE(in == decoded);
    }
}
