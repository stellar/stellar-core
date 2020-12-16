// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "lib/catch.hpp"
#include "main/Config.h"
#include "scp/QuorumSetUtils.h"
#include "test/test.h"
#include "util/Math.h"
#include <fmt/format.h>

using namespace stellar;

namespace
{

bool
keyMatches(PublicKey& key, const std::vector<std::string>& keys)
{
    auto keyStr = KeyUtils::toStrKey<PublicKey>(key);
    return std::any_of(std::begin(keys), std::end(keys),
                       [&](const std::string& x) { return keyStr == x; });
}
}

TEST_CASE("resolve node id", "[config]")
{
    auto cfg = getTestConfig(0);
    auto validator1Key =
        std::string{"GDKXE2OZMJIPOSLNA6N6F2BVCI3O777I2OOC4BV7VOYUEHYX7RTRYA7Y"};
    auto validator2Key =
        std::string{"GCUCJTIYXSOXKBSNFGNFWW5MUQ54HKRPGJUTQFJ5RQXZXNOLNXYDHRAP"};
    auto validator3Key =
        std::string{"GC2V2EFSXN6SQTWVYA5EPJPBWWIMSD2XQNKUOHGEKB535AQE2I6IXV2Z"};

    cfg.VALIDATOR_NAMES.emplace(std::make_pair(validator1Key, "core-testnet1"));
    cfg.VALIDATOR_NAMES.emplace(std::make_pair(validator2Key, "core-testnet2"));
    cfg.VALIDATOR_NAMES.emplace(std::make_pair(validator3Key, "core-testnet3"));

    SECTION("empty node id")
    {
        auto publicKey = PublicKey{};
        REQUIRE(!cfg.resolveNodeID("", publicKey));
    }

    SECTION("@")
    {
        auto publicKey = PublicKey{};
        REQUIRE(!cfg.resolveNodeID("@", publicKey));
    }

    SECTION("$")
    {
        auto publicKey = PublicKey{};
        REQUIRE(!cfg.resolveNodeID("@", publicKey));
    }

    SECTION("unique uppercase abbrevated id")
    {
        auto publicKey = PublicKey{};
        auto result = cfg.resolveNodeID("@GD", publicKey);
        REQUIRE(result);
        REQUIRE(keyMatches(publicKey, {validator1Key}));
    }

    SECTION("unique lowercase abbrevated id")
    {
        auto publicKey = PublicKey{};
        auto result = cfg.resolveNodeID("@gd", publicKey);
        REQUIRE(!result);
    }

    SECTION("non unique uppercase abbrevated id")
    {
        auto publicKey = PublicKey{};
        auto result = cfg.resolveNodeID("@GC", publicKey);
        REQUIRE(result);
        REQUIRE(keyMatches(publicKey, {validator2Key, validator3Key}));
    }

    SECTION("valid node alias")
    {
        auto publicKey = PublicKey{};
        auto result = cfg.resolveNodeID("$core-testnet1", publicKey);
        REQUIRE(result);
        REQUIRE(keyMatches(publicKey, {validator1Key}));
    }

    SECTION("uppercase node alias")
    {
        auto publicKey = PublicKey{};
        auto result = cfg.resolveNodeID("$CORE-TESTNET1", publicKey);
        REQUIRE(!result);
    }

    SECTION("node alias abbrevation")
    {
        auto publicKey = PublicKey{};
        auto result = cfg.resolveNodeID("$core", publicKey);
        REQUIRE(!result);
    }

    SECTION("existing full node id")
    {
        auto publicKey = PublicKey{};
        auto result = cfg.resolveNodeID(
            "GDKXE2OZMJIPOSLNA6N6F2BVCI3O777I2OOC4BV7VOYUEHYX7RTRYA7Y",
            publicKey);
        REQUIRE(result);
        REQUIRE(keyMatches(publicKey, {validator1Key}));
    }

    SECTION("abbrevated node id without prefix")
    {
        auto publicKey = PublicKey{};
        REQUIRE(!cfg.resolveNodeID("GDKXE2OZMJIPOSLNA6N6F2BVCI3O7", publicKey));
    }

    SECTION("existing lowercase full node id")
    {
        auto publicKey = PublicKey{};
        REQUIRE(!cfg.resolveNodeID(
            "gdkxe2ozmjiposlna6n6f2bvci3o777i2ooc4bv7voyuehyx7rtrya7y",
            publicKey));
    }

    SECTION("non existing full node id")
    {
        auto publicKey = PublicKey{};
        REQUIRE(!cfg.resolveNodeID(
            "SDTTOKJOEJXDBLATFZNTQRVA5MSCECMPOPC7CCCGL6AE5DKA7YCBJYJQ",
            publicKey));
    }

    SECTION("invalid key type")
    {
        auto publicKey = PublicKey{};
        REQUIRE(!cfg.resolveNodeID(
            "TDTTOKJOEJXDBLATFZNTQRVA5MSCECMPOPC7CCCGL6AE5DKA7YCBJYJQ",
            publicKey));
    }
}

TEST_CASE("load validators config", "[config]")
{
    Config c;
    c.load("testdata/stellar-core_example_validators.cfg");
    auto actualS = c.toString(c.QUORUM_SET);
    std::string expected = R"({
   "t" : 4,
   "v" : [
      {
         "t" : 3,
         "v" : [
            {
               "t" : 4,
               "v" : [
                  "i1",
                  "j1",
                  {
                     "t" : 3,
                     "v" : [
                        "h1",
                        "f1",
                        "g1",
                        {
                           "t" : 2,
                           "v" : [ "e1", "e2", "e3" ]
                        }
                     ]
                  },
                  {
                     "t" : 2,
                     "v" : [ "d2", "d1" ]
                  },
                  {
                     "t" : 2,
                     "v" : [ "c2", "c1" ]
                  }
               ]
            },
            {
               "t" : 2,
               "v" : [ "K1", "K2", "K3" ]
            },
            {
               "t" : 2,
               "v" : [ "a2", "a3", "a1" ]
            },
            {
               "t" : 2,
               "v" : [ "b1", "b2", "b3" ]
            }
         ]
      },
      {
         "t" : 2,
         "v" : [ "N3", "N1", "N2" ]
      },
      {
         "t" : 2,
         "v" : [ "L3", "L1", "L2" ]
      },
      {
         "t" : 2,
         "v" : [ "M1", "M2", "M3" ]
      }
   ]
}
)";

    REQUIRE(actualS == expected);
    REQUIRE(c.KNOWN_PEERS.size() == 15);
    REQUIRE(c.HISTORY.size() == 20);
}

TEST_CASE("bad validators configs", "[config]")
{
    // basic config has 4 top level as to meet safety requirement
    std::string const configPattern = R"(
NODE_SEED="SA7FGJMMUIHNE3ZPI2UO5I632A7O5FBAZTXFAIEVFA4DSSGLHXACLAIT a3"
{NODE_HOME_DOMAIN}
NODE_IS_VALIDATOR=true

############################
# list of HOME_DOMAINS
############################
[[HOME_DOMAINS]]
HOME_DOMAIN="domainA"
QUALITY="HIGH"

[[HOME_DOMAINS]]
HOME_DOMAIN="domainB"
QUALITY="HIGH"

[[HOME_DOMAINS]]
HOME_DOMAIN="domainC"
QUALITY="MEDIUM"

[[HOME_DOMAINS]]
QUALITY="HIGH"
HOME_DOMAIN="domainE"

[[HOME_DOMAINS]]
QUALITY="LOW"
HOME_DOMAIN="domainF"

############################
# List of Validators
############################
[[VALIDATORS]]
NAME="a1"
{A1_HOME_DOMAIN}
PUBLIC_KEY="GDUTST3TG4MNDLY6WLB5CIASIBZAWWWJKZDHA4HFEVKQOVTYQ2F5GKYZ"
{A1_QUALITY}
{A1_HISTORY}

[[VALIDATORS]]
NAME="a2"
HOME_DOMAIN="domainA"
PUBLIC_KEY="GBVZFVEARURUJTN5ABZPKW36FHKVJK2GHXEVY2SZCCNU5I3CQMTZ3OES"
HISTORY="curl a2"

[[VALIDATORS]]
NAME="b1"
HOME_DOMAIN="domainB"
PUBLIC_KEY="GCWA2U5S4EL4NJDU2FS4UUMNMUDUBCGGYEEY2DMOOLKT74AZR6RKWSMG"
HISTORY="curl b1"

[[VALIDATORS]]
NAME="b2"
HOME_DOMAIN="domainB"
PUBLIC_KEY="GDKL7ZSRR65GTILCE3BNO6XPCPAPGVYIMO3NIWSR6E7YS52KTJY6ZJL5"
HISTORY="curl b2"

[[VALIDATORS]]
NAME="b3"
HOME_DOMAIN="domainB"
PUBLIC_KEY="GDXSTS4LLXNPIA4YWRGMZZU625CLVUNIZQJYXMKBF2K6K4EXGANW46CJ"
HISTORY="curl b3"

[[VALIDATORS]]
NAME="K1"
HOME_DOMAIN="domainK"
QUALITY="HIGH"
PUBLIC_KEY="GBM2Y4JYEPVF2LAQ7KFOE6WBFOQLVVJETYRMIZYNSEGV6LNETOZHYXET"
HISTORY="curl k1"

[[VALIDATORS]]
NAME="K2"
HOME_DOMAIN="domainK"
QUALITY="HIGH"
PUBLIC_KEY="GBPD75VDOJVGMTTHTTU7NZ5DLGPKWAS3D7ZPQYCODTUKLE6N6JDWFECH"
HISTORY="curl k2"

[[VALIDATORS]]
NAME="K3"
HOME_DOMAIN="domainK"
QUALITY="HIGH"
PUBLIC_KEY="GBSGVVDQZ2JEHEQUYY5PLUZNSULTAKAVWIPA4J4ADRHAPFPKSZWZ3UVW"
ADDRESS="3.k"
HISTORY="curl k3"

[[VALIDATORS]]
NAME="c1"
HOME_DOMAIN="domainC"
PUBLIC_KEY="GDDKZKC6I4QHFJFIHQCET7FDOYK6B4D7FXCNCIJNI6REKOQZTQTKOZ7N"
HISTORY="curl c1"

{OTHER_VALIDATORS}
)";

    std::string const other = R"(
[[VALIDATORS]]
NAME="e1"
HOME_DOMAIN="domainE"
PUBLIC_KEY="GAU3H4KHUR54GWNHUJLEKT25RX6NVOZZGB55MLFWHWDFSP6H3QCQSMOA"
HISTORY="curl e1"

[[VALIDATORS]]
NAME="e2"
HOME_DOMAIN="domainE"
PUBLIC_KEY="GCBEPQHP3D42OHQMA54NRF3E4BAJ6T7NZP7Q7URI2VWNQDJPXTDA3SBJ"
HISTORY="curl e2"

[[VALIDATORS]]
NAME="e3"
HOME_DOMAIN="domainE"
PUBLIC_KEY="GDQ3GWCV4UH72MYZU5OCYVQCMIKMMKS6SWCI2OIRAIAISS5KCQCJTG37"
HISTORY="curl e3"

[[VALIDATORS]]
NAME="g1"
HOME_DOMAIN="domainG"
{QUALITY_G}
ADDRESS="1.g"
PUBLIC_KEY="GCWD2OTEJXLIDSOFSTWDPM5IDY27ZGEXIUIBGGA45Q2VXGQ2QAEBG7ZS"

)";

    std::string const nhd = "NODE_HOME_DOMAIN=\"domainA\"";
    std::string const hd = "HOME_DOMAIN=\"domainA\"";
    std::string const hist = "HISTORY=\"curl a1\"";
    std::string const qualG = "QUALITY=\"LOW\"";

    std::vector<std::array<std::string, 7>> tests = {
        {"sanity check", nhd, hd, hist, "", other, qualG},
        {"sanity check", nhd, hd, hist, "", "", ""},

        {"missing NODE_HOME_DOMAIN", "", hd, hist, "", other, qualG},
        {"missing HOME_DOMAIN", nhd, "", hist, "", other, qualG},
        {"HIGH must have archive", nhd, hd, "", "", other, qualG},
        {"quality already defined", nhd, hd, hist, "QUALITY=\"HIGH\"", other,
         qualG},
        {"need 3 for HIGH", "NODE_HOME_DOMAIN=\"domainAA\"", hd, hist, "",
         other, qualG},
        {"unknown quality", nhd, hd, hist, "", other, "QUALITY=\"OTHER\""},
        {"missing quality", nhd, hd, hist, "", other, ""}};
    int i = 0;
    for (auto const& t : tests)
    {
        ++i;
        DYNAMIC_SECTION(t[0] << " " << i)
        {
            auto other2 = fmt::format(t[5], fmt::arg("QUALITY_G", t[6]));
            auto newConfig = fmt::format(
                configPattern, fmt::arg("NODE_HOME_DOMAIN", t[1]),
                fmt::arg("A1_HOME_DOMAIN", t[2]), fmt::arg("A1_HISTORY", t[3]),
                fmt::arg("A1_QUALITY", t[4]),
                fmt::arg("OTHER_VALIDATORS", other2));
            std::stringstream ss(newConfig);
            Config c;
            if (t[0] == "sanity check")
            {
                c.load(ss);
            }
            else
            {
                REQUIRE_THROWS(c.load(ss));
            }
        }
    }
}

TEST_CASE("load example configs", "[config]")
{
    Config c;
    std::vector<std::string> testFiles = {
        "stellar-core_example.cfg", "stellar-core_standalone.cfg",
        "stellar-core_testnet_legacy.cfg", "stellar-core_testnet.cfg"};
    for (auto const& fn : testFiles)
    {
        std::string fnPath = "testdata/";
        fnPath += fn;
        SECTION("load config " + fnPath)
        {
            c.load(fnPath);
        }
    }
}

TEST_CASE("nesting level", "[config]")
{
    auto makePublicKey = [](int i) {
        auto hash = sha256(fmt::format("NODE_SEED_{}", i));
        auto secretKey = SecretKey::fromSeed(hash);
        return secretKey.getStrKeyPublic();
    };
    std::string configNesting = "UNSAFE_QUORUM=true";
    std::string quorumSetNumber = "";
    std::string quorumSetTemplate = R"(

[QUORUM_SET{}]
THRESHOLD_PERCENT=50
VALIDATORS=[
    "{} {}",
    "{} {}"
]
)";
    for (uint32 nestingLevel = 0; nestingLevel < 10; nestingLevel++)
    {
        configNesting += fmt::format(
            quorumSetTemplate, quorumSetNumber, makePublicKey(nestingLevel * 2),
            char('A' + nestingLevel * 2), makePublicKey(nestingLevel * 2 + 1),
            char('A' + nestingLevel * 2 + 1));
        SECTION(fmt::format("nesting level = {}", nestingLevel))
        {
            Config c;
            std::stringstream ss(configNesting);
            if (nestingLevel <= MAXIMUM_QUORUM_NESTING_LEVEL)
            {
                REQUIRE_NOTHROW(c.load(ss));
            }
            else
            {
                REQUIRE_THROWS(c.load(ss));
            }
        }
        quorumSetNumber += ".1";
    }
}

TEST_CASE("operation filter configuration", "[config]")
{
    typedef xdr::xdr_traits<OperationType> OperationTypeTraits;
    auto toConfigStr = [](std::vector<OperationType> const& vals,
                          std::stringstream& ss) {
        ss << "EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE=[";
        auto iter = vals.begin();
        if (iter != vals.end())
        {
            while (iter != vals.end() - 1)
            {
                ss << "\"" << OperationTypeTraits::enum_name(*iter++) << "\", ";
            }
            ss << "\"" << OperationTypeTraits::enum_name(*iter++) << "\"";
        }
        ss << "]";
        CLOG_ERROR(Tx, "{}", ss.str());
    };

    auto loadConfig = [&](std::vector<OperationType> const& vals) {
        auto makePublicKey = [](int i) {
            auto hash = sha256(fmt::format("NODE_SEED_{}", i));
            auto secretKey = SecretKey::fromSeed(hash);
            return secretKey.getStrKeyPublic();
        };

        std::stringstream ss;
        ss << "UNSAFE_QUORUM=true\n";
        toConfigStr(vals, ss);
        ss << "\n[QUORUM_SET]\n";
        ss << "THRESHOLD_PERCENT=100\n";
        ss << "VALIDATORS=[\"" << makePublicKey(0) << " A\"]\n";

        Config c;
        REQUIRE_NOTHROW(c.load(ss));
        auto const& exclude = c.EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE;
        REQUIRE(exclude == vals);
    };

    // Test every operation type individually
    for (auto v : OperationTypeTraits::enum_values())
    {
        std::vector<OperationType> vals;
        vals.emplace_back(static_cast<OperationType>(v));
        loadConfig(vals);
    }

    // Test random subsets that are not necessarily in the typical order
    std::uniform_int_distribution<size_t> dist(
        0, OperationTypeTraits::enum_values().size() - 1);
    for (size_t i = 0; i < 5; ++i)
    {
        std::vector<OperationType> vals;
        for (auto v : OperationTypeTraits::enum_values())
        {
            vals.emplace_back(static_cast<OperationType>(v));
        }
        std::shuffle(vals.begin(), vals.end(), gRandomEngine);
        vals.resize(dist(gRandomEngine));
        loadConfig(vals);
    }
}
