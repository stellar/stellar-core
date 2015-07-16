// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/test.h"
#include "util/Logging.h"
#include "lib/catch.hpp"
#include "crypto/Base58.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "crypto/Random.h"
#include "crypto/StrKey.h"
#include <autocheck/autocheck.hpp>
#include <sodium.h>
#include <map>
#include <regex>

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
    LOG(DEBUG) << "k1: " << k1.getBase58Seed();
    LOG(DEBUG) << "k2: " << k2.getBase58Seed();
    CHECK(k1.getBase58Seed() != k2.getBase58Seed());
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
        [](std::vector<uint8_t> v)
        {
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

// Note: the fixed test vectors are based on the bitcoin alphabet; the stellar /
// ripple alphabet is a permutation of it. But these ought to test the algorithm
// relatively well and have been cross-checked against several implementations
// in different languages. There aren't a lot of independent implementations
// that speak the ripple alphabet.

static std::map<std::vector<uint8_t>, std::string> base58TestVectors = {
    {{97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97,
      97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97},
     "7Z8ftDAzMvoyXnGEJye8DurzgQQXLAbYCaeeesM7UKHa"},

    {{97, 98, 99, 100, 97, 98, 99, 100, 97, 98, 99, 100, 97, 98, 99, 100, 97,
      98, 99, 100, 97, 98, 99, 100, 97, 98, 99, 100, 97, 98, 99, 100},
     "7Z9ZajDvyzs9sYf85A9gAAYxcmHYSbWsGNLrZ3rzLAeP"},

    {{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x1a, 0x1b,
      0x1c, 0x1d, 0x1e, 0x1f},
     "12drXXUifSrRnfLCV62Ht"},

    {{}, ""},
    {{0}, "1"},
    {{0, 0}, "11"},
    {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0},
     "11111111111111111111111111111111"},

    {{0xff}, "5Q"},
    {{0xff, 0xff}, "LUv"},
    {{0xff, 0xff, 0xff}, "2UzHL"},
    {{1}, "2"},
    {{1, 1}, "5S"},

    {{0x01, 0x01, 0xff, 0x00}, "2VfAo"},

    {{0xb4, 0xda, 0x4a, 0x70, 0xa7, 0x61, 0xca, 0x41, 0x69, 0x33, 0x5d, 0xc0,
      0x2b, 0xd3, 0xa6, 0x58},
     "PLHQNH1Kpm1w5WN9QSQJko"},

    {{0x52, 0xdf, 0x8c, 0xa2, 0x80, 0xa7, 0xd, 0xa1, 0x3d, 0xc0, 0xf8, 0x76,
      0x0, 0x80, 0x3e, 0x81},
     "BEYde8cpJw3kKZEX29eWaC"},

    {{0x2f, 0x28, 0xed, 0xfc, 0xae, 0x85, 0x7, 0xaf, 0xf, 0x4a, 0xec, 0xbd,
      0x6a, 0x98, 0x55, 0xbb},
     "6pmGMkyWgwasgS1VmiM4U2"},

    {{0xdb, 0x95, 0xc5, 0x32, 0x28, 0x43, 0xdc, 0x9b, 0xb2, 0x34, 0xc3, 0x23,
      0x30, 0xfc, 0xa5, 0x11},
     "U7grozkGcCERSK7owUsJXa"},

    {{0xc4, 0x2a, 0x64, 0xc, 0x71, 0xf7, 0x22, 0xdd, 0x4a, 0x93, 0x6c, 0xa1,
      0xa3, 0x1b, 0x51, 0x82},
     "RDxPrFYS9Cru3n79e6ahi1"},

    {{0xe1, 0xc1, 0x7c, 0x47, 0x5a, 0x82, 0x43, 0x55, 0x6c, 0xd5, 0x5b, 0x12,
      0xb6, 0x98, 0x1c, 0x83},
     "UstCbvfvLMCshNmbGSGYnn"},
};

TEST_CASE("base58 tests", "[crypto]")
{
    // Do some fixed test vectors.
    for (auto const& pair : base58TestVectors)
    {
        LOG(DEBUG) << "fixed test vector base58: \"" << pair.second << "\"";

        auto enc = baseEncode(bitcoinBase58Alphabet, pair.first);
        CHECK(enc == pair.second);

        auto dec = baseDecode(bitcoinBase58Alphabet, pair.second);
        CHECK(pair.first == dec);
    }

    // Do 20 random round-trip tests.
    autocheck::check<std::vector<uint8_t>>(
        [](std::vector<uint8_t> v)
        {
            auto enc = baseEncode(bitcoinBase58Alphabet, v);
            auto dec = baseDecode(bitcoinBase58Alphabet, enc);
            LOG(DEBUG) << "random round-trip base58: \"" << enc << "\"";
            CHECK(v == dec);
            return v == dec;
        },
        20);

    // Do 20 random round-trip tests on the stellar alphabet.
    autocheck::check<std::vector<uint8_t>>(
        [](std::vector<uint8_t> v)
        {
            auto enc = baseEncode(stellarBase58Alphabet, v);
            auto dec = baseDecode(stellarBase58Alphabet, enc);
            LOG(DEBUG) << "random round-trip stellar base58: \"" << enc << "\"";
            CHECK(v == dec);
            return v == dec;
        },
        20);
}

static std::map<std::vector<uint8_t>, std::string> base58CheckTestVectors = {

    {{0xd3, 0xc8, 0xe7, 0xba, 0x6, 0x31, 0x66, 0x2d, 0x9b, 0x3b, 0x54, 0x4,
      0x11, 0xe7, 0x3c, 0xf1, 0x11, 0xe7, 0x3c, 0xf1},
     "1LJpFcZ8yj1rdfkUMwkYbT6RWfn4C2CSus"},

    {{0xae, 0x82, 0xcf, 0x4e, 0xbb, 0xdc, 0x62, 0x18, 0xe1, 0xbe, 0xf4, 0xb4,
      0x5e, 0x2e, 0xf7, 0x13, 0x5e, 0x2e, 0xf7, 0x13},
     "1GujHCjs1kgRnW4XBEuEQojFucTwSuQSWS"},

    {{0xab, 0xf1, 0xa, 0xbc, 0x85, 0xb0, 0xb0, 0x47, 0xc, 0x0, 0x49, 0x55, 0xb9,
      0x7b, 0xf4, 0xa0, 0xa, 0xbc, 0x85, 0xb0},
     "1Gg9Jh3z3ZokEYRspgV212iyQsbUXQTz2i"},
};

TEST_CASE("base58check tests", "[crypto]")
{
    // Do some fixed test vectors.
    for (auto const& pair : base58CheckTestVectors)
    {
        LOG(DEBUG) << "fixed test vector base58check: \"" << pair.second
                   << "\"";

        auto enc = baseCheckEncode(bitcoinBase58Alphabet, 0, pair.first);
        CHECK(enc == pair.second);

        auto dec = baseCheckDecode(bitcoinBase58Alphabet, pair.second);
        CHECK(0 == dec.first);
        CHECK(pair.first == dec.second);
    }

    // Do 20 random round-trip tests on the stellar alphabet.
    autocheck::check<std::vector<uint8_t>, uint8_t>(
        [](std::vector<uint8_t> bytes, uint8_t ver)
        {
            auto enc =
                toBase58Check(static_cast<Base58CheckVersionByte>(ver), bytes);
            auto dec = fromBase58Check(enc);
            LOG(DEBUG) << "random round-trip stellar base58check: \"" << enc
                       << "\"";
            CHECK(ver == dec.first);
            CHECK(bytes == dec.second);
            return ver == dec.first && bytes == dec.second;
        },
        20);
}

TEST_CASE("sign tests", "[crypto]")
{
    auto sk = SecretKey::random();
    auto pk = sk.getPublicKey();
    LOG(DEBUG) << "generated random secret key seed: " << sk.getBase58Seed();
    LOG(DEBUG) << "corresponding public key: " << PubKeyUtils::toBase58(pk);

    CHECK(SecretKey::fromBase58Seed(sk.getBase58Seed()) == sk);

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

TEST_CASE("StrKey tests")
{
    std::regex b32("^([A-Z2-7])+$");
    std::regex b32Pad("^([A-Z2-7])+(=|===|====|======)?$");

    autocheck::generator<std::vector<uint8_t>> input;

    uint8_t version = 2;

    // check round trip
    for (int size = 0; size < 100; size++)
    {
        std::vector<uint8_t> in(input(size));

        std::string encoded = strKey::toStrKey(version, in);

        REQUIRE(encoded.size() == ((size + 3 + 4)/5 * 8));

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
    for (int round = 0; round < 5; round++)
    {
        const int expectedSize = 32;
        std::vector<uint8_t> in(input(expectedSize));
        std::string encoded = strKey::toStrKey(version, in);

        for (int p = 0; p < encoded.size(); p++)
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
                bool res = !strKey::fromStrKey(corrupted, ver, dt);
                REQUIRE(res);
            }
        }
    }
}
