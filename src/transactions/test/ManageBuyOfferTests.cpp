// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/TrustLineWrapper.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/ManageBuyOfferOpFrame.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "xdrpp/autocheck.h"

using namespace stellar;
using namespace stellar::txtest;

void
for_current_and_previous_version_from(size_t minVersion, Application& app,
                                      std::function<void(void)> const& f)
{
    REQUIRE(Config::CURRENT_LEDGER_PROTOCOL_VERSION >= 1);
    uint32_t const currentVersion = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    uint32_t const previousVersion = currentVersion - 1;
    if (minVersion <= previousVersion)
    {
        for_versions({currentVersion, previousVersion}, app, f);
    }
    else if (minVersion <= currentVersion)
    {
        for_versions({currentVersion}, app, f);
    }
}

TEST_CASE("manage buy offer failure modes", "[tx][offers]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    int64_t const txfee = app->getLedgerManager().getLastTxFee();
    int64_t const minBalancePlusFees =
        app->getLedgerManager().getLastMinBalance(0) + 100 * txfee;
    int64_t const minBalance1PlusFees =
        app->getLedgerManager().getLastMinBalance(1) + 100 * txfee;
    int64_t const minBalance3PlusFees =
        app->getLedgerManager().getLastMinBalance(3) + 100 * txfee;
    int64_t const minBalance4PlusFees =
        app->getLedgerManager().getLastMinBalance(4) + 100 * txfee;

    auto root = TestAccount::createRoot(*app);
    auto issuer1 = root.create("issuer1", minBalancePlusFees);
    auto issuer2 = root.create("issuer2", minBalancePlusFees);

    auto native = makeNativeAsset();
    auto cur1 = issuer1.asset("CUR1");
    auto cur2 = issuer2.asset("CUR2");

    SECTION("not supported before version 11")
    {
        for_versions({10}, *app, [&]() {
            auto a1 = root.create("a1", minBalance1PlusFees);
            REQUIRE_THROWS_AS(a1.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 1),
                              ex_opNOT_SUPPORTED);
        });
    }

    for_current_and_previous_version_from(11, *app, [&]() {
        SECTION("check valid")
        {
            Asset invalid(ASSET_TYPE_CREDIT_ALPHANUM4);
            strToAssetCode(invalid.alphaNum4().assetCode, "");

            SECTION("selling asset not valid")
            {
                auto a1 = root.create("a1", minBalance1PlusFees);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, invalid, native, Price{1, 1}, 1),
                    ex_MANAGE_BUY_OFFER_MALFORMED);
            }

            SECTION("buying asset not valid")
            {
                auto a1 = root.create("a1", minBalance1PlusFees);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, native, invalid, Price{1, 1}, 1),
                    ex_MANAGE_BUY_OFFER_MALFORMED);
            }

            SECTION("buying and selling same asset")
            {
                auto a1 = root.create("a1", minBalance1PlusFees);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, cur1, Price{1, 1}, 1),
                    ex_MANAGE_BUY_OFFER_MALFORMED);
            }

            SECTION("negative amount")
            {
                auto a1 = root.create("a1", minBalance1PlusFees);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, cur2, Price{1, 1}, -1),
                    ex_MANAGE_BUY_OFFER_MALFORMED);
            }

            SECTION("non-positive price numerator")
            {
                auto a1 = root.create("a1", minBalance1PlusFees);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, cur2, Price{0, 1}, 1),
                    ex_MANAGE_BUY_OFFER_MALFORMED);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, cur2, Price{-1, 1}, 1),
                    ex_MANAGE_BUY_OFFER_MALFORMED);
            }

            SECTION("non-positive price denominator")
            {
                auto a1 = root.create("a1", minBalance1PlusFees);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, cur2, Price{1, 0}, 1),
                    ex_MANAGE_BUY_OFFER_MALFORMED);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, cur2, Price{1, -1}, 1),
                    ex_MANAGE_BUY_OFFER_MALFORMED);
            }

            SECTION("delete and create")
            {
                auto a1 = root.create("a1", minBalance1PlusFees);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 0),
                    ex_MANAGE_BUY_OFFER_MALFORMED);
            }
        }

        SECTION("check offer valid")
        {
            uint32_t ledgerVersion;
            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                ledgerVersion = ltx.loadHeader().current().ledgerVersion;
            }

            SECTION("no issuer")
            {
                auto a1 = root.create("a1", minBalance3PlusFees);
                a1.changeTrust(cur1, INT64_MAX);
                issuer1.pay(a1, cur1, 1);
                closeLedgerOn(*app, 2, 1, 1, 2016);

                // remove issuer
                issuer1.merge(root);

                SECTION("sell no issuer")
                {
                    if (ledgerVersion < 13)
                    {
                        REQUIRE_THROWS_AS(
                            a1.manageBuyOffer(0, cur1, native, Price{1, 1}, 1),
                            ex_MANAGE_BUY_OFFER_SELL_NO_ISSUER);
                    }
                    else
                    {
                        a1.manageBuyOffer(0, cur1, native, Price{1, 1}, 1);
                    }
                }

                SECTION("buy no issuer")
                {
                    if (ledgerVersion < 13)
                    {
                        REQUIRE_THROWS_AS(
                            a1.manageBuyOffer(0, native, cur1, Price{1, 1}, 1),
                            ex_MANAGE_BUY_OFFER_BUY_NO_ISSUER);
                    }
                    else
                    {
                        a1.manageBuyOffer(0, native, cur1, Price{1, 1}, 1);
                    }
                }
            }

            SECTION("sell no trust")
            {
                auto a1 = root.create("a1", minBalance1PlusFees);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, native, Price{1, 1}, 1),
                    ex_MANAGE_BUY_OFFER_SELL_NO_TRUST);
            }

            SECTION("sell no balance")
            {
                auto a1 = root.create("a1", minBalance1PlusFees);
                a1.changeTrust(cur1, INT64_MAX);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, native, Price{1, 1}, 1),
                    ex_MANAGE_BUY_OFFER_UNDERFUNDED);
            }

            SECTION("sell not authorized")
            {
                auto toSet = static_cast<uint32_t>(AUTH_REQUIRED_FLAG) |
                             static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
                issuer1.setOptions(setFlags(toSet));
                auto a1 = root.create("a1", minBalance1PlusFees);
                a1.changeTrust(cur1, INT64_MAX);
                issuer1.allowTrust(cur1, a1);
                issuer1.pay(a1, cur1, 1);
                issuer1.denyTrust(cur1, a1);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, native, Price{1, 1}, 1),
                    ex_MANAGE_BUY_OFFER_SELL_NOT_AUTHORIZED);
            }

            SECTION("buy no trust")
            {
                auto a1 = root.create("a1", minBalance1PlusFees);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, native, cur2, Price{1, 1}, 1),
                    ex_MANAGE_BUY_OFFER_BUY_NO_TRUST);
            }

            SECTION("buy not authorized")
            {
                auto toSet = static_cast<uint32_t>(AUTH_REQUIRED_FLAG);
                issuer2.setOptions(setFlags(toSet));
                auto a1 = root.create("a1", minBalance1PlusFees);
                a1.changeTrust(cur2, INT64_MAX);
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, native, cur2, Price{1, 1}, 1),
                    ex_MANAGE_BUY_OFFER_BUY_NOT_AUTHORIZED);
            }
        }

        SECTION("offer must exist and be owned by source account to modify or "
                "delete")
        {
            auto a1 = root.create("a1", minBalance3PlusFees);
            a1.changeTrust(cur1, INT64_MAX);
            a1.changeTrust(cur2, INT64_MAX);
            issuer1.pay(a1, cur1, 1);
            issuer2.pay(a1, cur2, 1);
            // Shows that the offer cannot be updated if it does not exist
            REQUIRE_THROWS_AS(a1.manageBuyOffer(1, cur1, cur2, Price{1, 1}, 1),
                              ex_MANAGE_BUY_OFFER_NOT_FOUND);

            auto a2 = root.create("a2", minBalance3PlusFees);
            a2.changeTrust(cur1, INT64_MAX);
            a2.changeTrust(cur2, INT64_MAX);
            issuer1.pay(a2, cur1, 1);
            issuer2.pay(a2, cur2, 1);
            REQUIRE(a2.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 1) == 1);
            // Shows that the offer can be updated if it does exist
            REQUIRE(a2.manageBuyOffer(1, cur1, cur2, Price{1, 1}, 1,
                                      MANAGE_OFFER_UPDATED) == 1);

            // Shows that the offer must be owned by the source account to be
            // updated
            REQUIRE_THROWS_AS(a1.manageBuyOffer(1, cur1, cur2, Price{1, 1}, 1),
                              ex_MANAGE_BUY_OFFER_NOT_FOUND);
        }

        SECTION("compute offer exchange parameters")
        {
            SECTION("reserve")
            {
                const int64_t minBalance =
                    app->getLedgerManager().getLastMinBalance(3) + 3 * txfee;

                auto a1 = root.create("a1", minBalance - 1);
                a1.changeTrust(cur1, INT64_MAX);
                a1.changeTrust(cur2, INT64_MAX);
                issuer1.pay(a1, cur1, 1);
                issuer2.pay(a1, cur2, 1);
                // Shows that 1 below required reserve will fail
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 1),
                    ex_MANAGE_BUY_OFFER_LOW_RESERVE);

                auto a2 = root.create("a2", minBalance);
                a2.changeTrust(cur1, INT64_MAX);
                a2.changeTrust(cur2, INT64_MAX);
                issuer1.pay(a2, cur1, 1);
                issuer2.pay(a2, cur2, 1);
                // Shows that required reserve will succeed
                REQUIRE(a2.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 1) == 1);
            }

            SECTION("buying liabilities")
            {
                auto a1 = root.create("a1", minBalance4PlusFees);
                a1.changeTrust(cur1, INT64_MAX);
                a1.changeTrust(cur2, 2000);
                issuer1.pay(a1, cur1, INT64_MAX);
                issuer2.pay(a1, cur2, 1000);
                REQUIRE(a1.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 500) ==
                        1);

                // Shows that 1 above available limit fails
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 501),
                    ex_MANAGE_BUY_OFFER_LINE_FULL);
                // Shows that available limit will succeed
                REQUIRE(a1.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 500) ==
                        2);
            }

            SECTION("selling liabilities")
            {
                auto a1 = root.create("a1", minBalance4PlusFees);
                a1.changeTrust(cur1, INT64_MAX);
                a1.changeTrust(cur2, INT64_MAX);
                issuer1.pay(a1, cur1, 1000);
                REQUIRE(a1.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 500) ==
                        1);

                // Shows that 1 above available balance fails
                REQUIRE_THROWS_AS(
                    a1.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 501),
                    ex_MANAGE_BUY_OFFER_UNDERFUNDED);
                // Shows that available balance will succeed
                REQUIRE(a1.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 500) ==
                        2);
            }
        }
    });

    SECTION("negative offerID")
    {
        for_versions({11, 12, 13, 14}, *app, [&]() {
            REQUIRE_THROWS_AS(
                issuer1.manageBuyOffer(-1, cur1, native, Price{1, 1}, 1),
                ex_MANAGE_BUY_OFFER_NOT_FOUND);
        });

        for_versions_from(15, *app, [&]() {
            REQUIRE_THROWS_AS(
                issuer1.manageBuyOffer(-1, cur1, native, Price{1, 1}, 1),
                ex_MANAGE_BUY_OFFER_MALFORMED);
        });
    }
}

TEST_CASE("manage buy offer liabilities", "[tx][offers]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    auto checkLiabilities = [&](std::string const& section, int64_t buyAmount,
                                Price const& price, int64_t expectedBuying,
                                int64_t expectedSelling) {
        SECTION(section)
        {
            auto cur1 = autocheck::generator<Asset>()(5);
            auto cur2 = cur1;
            while (cur2 == cur1)
            {
                cur2 = autocheck::generator<Asset>()(5);
            }

            auto op = manageBuyOffer(0, cur1, cur2, price, buyAmount);
            auto tx = transactionFromOperations(
                *app, SecretKey::pseudoRandomForTesting(), 1, {op});

            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                tx->checkValid(ltx, 0, 0, 0);
            }

            auto buyOp = std::static_pointer_cast<ManageBuyOfferOpFrame>(
                tx->getOperations().front());
            REQUIRE(expectedBuying == buyOp->getOfferBuyingLiabilities());
            REQUIRE(expectedSelling == buyOp->getOfferSellingLiabilities());
        }
    };

    for_current_and_previous_version_from(11, *app, [&]() {
        SECTION("wheat stays")
        {
            SECTION("with no rounding")
            {
                checkLiabilities("buy five for two", 20, Price{2, 5}, 20, 8);
                checkLiabilities("buy two for one", 20, Price{1, 2}, 20, 10);
                checkLiabilities("buy one for one", 20, Price{1, 1}, 20, 20);
                checkLiabilities("buy one for two", 20, Price{2, 1}, 20, 40);
                checkLiabilities("buy two for five", 20, Price{5, 2}, 20, 50);
            }

            SECTION("with rounding")
            {
                checkLiabilities("buy five for two", 21, Price{2, 5}, 20, 8);
                checkLiabilities("buy two for one", 21, Price{1, 2}, 20, 10);
                checkLiabilities("buy one for one", 21, Price{2, 1}, 21, 42);
                checkLiabilities("buy two for five", 21, Price{5, 2}, 21, 53);
            }
        }

        // Note: This can only happen with price.n >= price.d
        SECTION("sheep stays")
        {
            SECTION("with no rounding")
            {
                checkLiabilities("buy one for one", INT64_MAX, Price{1, 1},
                                 INT64_MAX, INT64_MAX);
            }

            SECTION("with rounding")
            {
                checkLiabilities("buy one for two", INT64_MAX, Price{2, 1},
                                 bigDivide((uint128_t)INT64_MAX, 2, ROUND_DOWN),
                                 INT64_MAX - 1);
                checkLiabilities("buy two for five", INT64_MAX, Price{5, 2},
                                 bigDivide(INT64_MAX, 2, 5, ROUND_DOWN),
                                 INT64_MAX - 2);
            }
        }

        checkLiabilities("zero liabilities", 1, Price{1, 2}, 0, 0);
    });
}

TEST_CASE("manage buy offer exactly crosses existing offers", "[tx][offers]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    int64_t const txfee = app->getLedgerManager().getLastTxFee();
    int64_t const minBalancePlusFees =
        app->getLedgerManager().getLastMinBalance(0) + 100 * txfee;
    int64_t const minBalance3PlusFees =
        app->getLedgerManager().getLastMinBalance(3) + 100 * txfee;

    auto root = TestAccount::createRoot(*app);
    auto issuer = root.create("issuer", minBalancePlusFees);
    auto a1 = root.create("a1", minBalance3PlusFees);
    auto a2 = root.create("a2", minBalance3PlusFees);

    auto native = makeNativeAsset();
    auto cur1 = issuer.asset("CUR1");
    auto cur2 = issuer.asset("CUR2");

    a1.changeTrust(cur1, INT64_MAX);
    issuer.pay(a1, cur1, INT64_MAX);
    a1.changeTrust(cur2, INT64_MAX);

    a2.changeTrust(cur1, INT64_MAX);
    a2.changeTrust(cur2, INT64_MAX);
    issuer.pay(a2, cur2, INT64_MAX);

    auto doTest = [&](std::string const& section, Price const& price,
                      int64_t amount) {
        SECTION(section)
        {
            auto offerID = a1.manageOffer(0, cur1, cur2, price, amount);
            a2.manageBuyOffer(0, cur2, cur1, price, amount,
                              MANAGE_OFFER_DELETED);

            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(!stellar::loadOffer(ltx, a1.getPublicKey(), offerID));
        }
    };

    doTest("buy five for two", Price{2, 5}, 20);
    doTest("buy two for one", Price{1, 2}, 20);
    doTest("buy one for one", Price{1, 1}, 20);
    doTest("buy one for two", Price{2, 1}, 20);
    doTest("buy two for five", Price{5, 2}, 20);
}

TEST_CASE("manage buy offer matches manage sell offer when not executing",
          "[tx][offers]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    int64_t const txfee = app->getLedgerManager().getLastTxFee();
    int64_t const minBalancePlusFees =
        app->getLedgerManager().getLastMinBalance(0) + 100 * txfee;
    int64_t const minBalance3PlusFees =
        app->getLedgerManager().getLastMinBalance(3) + 100 * txfee;

    auto root = TestAccount::createRoot(*app);
    auto issuer = root.create("issuer", minBalancePlusFees);
    auto a1 = root.create("a1", minBalance3PlusFees);
    auto a2 = root.create("a2", minBalance3PlusFees);

    auto native = makeNativeAsset();
    auto cur1 = issuer.asset("CUR1");
    auto cur2 = issuer.asset("CUR2");
    auto cur3 = issuer.asset("CUR3");
    auto cur4 = issuer.asset("CUR4");

    int64_t const availableBalance = 100;
    int64_t const availableLimit = 100;

    auto checkOffer = [&](AccountID const& acc, int64_t offerID,
                          Asset const& selling, Asset const& buying,
                          Price const& price) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto offer = stellar::loadOffer(ltx, acc, offerID);
        auto const& oe = offer.current().data.offer();
        REQUIRE(oe.selling == selling);
        REQUIRE(oe.buying == buying);
        REQUIRE(oe.price == price);
        return oe.amount;
    };

    auto getManageBuyOfferAmount = [&](Price const& price, int64_t buyAmount) {
        a1.changeTrust(cur1, availableBalance);
        issuer.pay(a1, cur1, availableBalance);
        a1.changeTrust(cur2, availableLimit);

        auto offerID = a1.manageBuyOffer(0, cur1, cur2, price, buyAmount);
        return checkOffer(a1.getPublicKey(), offerID, cur1, cur2,
                          Price{price.d, price.n});
    };

    auto getManageSellOfferAmount = [&](Price const& price, int64_t amount) {
        a2.changeTrust(cur3, availableBalance);
        issuer.pay(a2, cur3, availableBalance);
        a2.changeTrust(cur4, availableLimit);

        auto offerID = a2.manageOffer(0, cur3, cur4, price, amount);
        return checkOffer(a2.getPublicKey(), offerID, cur3, cur4, price);
    };

    for_current_and_previous_version_from(11, *app, [&]() {
        SECTION("with no rounding")
        {
            SECTION("sell two for five")
            {
                REQUIRE(getManageBuyOfferAmount(Price{2, 5}, 20) ==
                        getManageSellOfferAmount(Price{5, 2}, 8));
            }
            SECTION("sell one for two")
            {
                REQUIRE(getManageBuyOfferAmount(Price{1, 2}, 20) ==
                        getManageSellOfferAmount(Price{2, 1}, 10));
            }
            SECTION("sell one for one")
            {
                REQUIRE(getManageBuyOfferAmount(Price{1, 1}, 20) ==
                        getManageSellOfferAmount(Price{1, 1}, 20));
            }
            SECTION("sell two for one")
            {
                REQUIRE(getManageBuyOfferAmount(Price{2, 1}, 20) ==
                        getManageSellOfferAmount(Price{1, 2}, 40));
            }
            SECTION("sell five for two")
            {
                REQUIRE(getManageBuyOfferAmount(Price{5, 2}, 20) ==
                        getManageSellOfferAmount(Price{2, 5}, 50));
            }
        }

        SECTION("with rounding")
        {
            SECTION("sell two for five")
            {
                REQUIRE(getManageBuyOfferAmount(Price{2, 5}, 21) ==
                        getManageSellOfferAmount(Price{5, 2}, 8));
            }
            SECTION("sell one for two")
            {
                REQUIRE(getManageBuyOfferAmount(Price{1, 2}, 21) ==
                        getManageSellOfferAmount(Price{2, 1}, 10));
            }
            SECTION("sell two for one")
            {
                REQUIRE(getManageBuyOfferAmount(Price{2, 1}, 21) ==
                        getManageSellOfferAmount(Price{1, 2}, 42));
            }
            SECTION("sell five for two")
            {
                REQUIRE(getManageBuyOfferAmount(Price{5, 2}, 21) ==
                        getManageSellOfferAmount(Price{2, 5}, 53));
            }
        }
    });
}

TEST_CASE("manage buy offer matches manage sell offer when executing partially",
          "[tx][offers]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    int64_t const txfee = app->getLedgerManager().getLastTxFee();
    int64_t const minBalancePlusFees =
        app->getLedgerManager().getLastMinBalance(0) + 100 * txfee;
    int64_t const minBalance3PlusFees =
        app->getLedgerManager().getLastMinBalance(3) + 100 * txfee;

    auto root = TestAccount::createRoot(*app);
    auto issuer = root.create("issuer", minBalancePlusFees);
    auto a1 = root.create("a1", minBalance3PlusFees);
    auto a2 = root.create("a2", minBalance3PlusFees);
    auto a3 = root.create("a3", minBalance3PlusFees);
    auto a4 = root.create("a4", minBalance3PlusFees);

    auto native = makeNativeAsset();
    auto cur1 = issuer.asset("CUR1");
    auto cur2 = issuer.asset("CUR2");
    auto cur3 = issuer.asset("CUR3");
    auto cur4 = issuer.asset("CUR4");

    int64_t const availableBalance = 1000;
    int64_t const availableLimit = 1000;
    int64_t const offerSizeInLedger = 50;

    auto checkOffer = [&](AccountID const& acc, int64_t offerID,
                          Asset const& selling, Asset const& buying,
                          Price const& price) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto offer = stellar::loadOffer(ltx, acc, offerID);
        auto const& oe = offer.current().data.offer();
        REQUIRE(oe.selling == selling);
        REQUIRE(oe.buying == buying);
        REQUIRE(oe.price == price);
        return oe.amount;
    };

    auto checkTrustLine = [&](AccountID const& acc, Asset const& asset) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto trust = stellar::loadTrustLine(ltx, acc, asset);
        return trust.getBalance();
    };

    auto getManageBuyOfferAmount = [&](Price const& price, int64_t buyAmount) {
        a1.changeTrust(cur1, availableBalance);
        issuer.pay(a1, cur1, availableBalance);
        a1.changeTrust(cur2, availableLimit);

        a2.changeTrust(cur2, INT64_MAX);
        issuer.pay(a2, cur2, INT64_MAX);
        a2.changeTrust(cur1, INT64_MAX);

        a2.manageOffer(0, cur2, cur1, price, offerSizeInLedger);
        auto offerID = a1.manageBuyOffer(0, cur1, cur2, price, buyAmount);
        return std::make_tuple(availableBalance -
                                   checkTrustLine(a1.getPublicKey(), cur1),
                               checkTrustLine(a1.getPublicKey(), cur2),
                               checkOffer(a1.getPublicKey(), offerID, cur1,
                                          cur2, Price{price.d, price.n}));
    };

    auto getManageSellOfferAmount = [&](Price const& price,
                                        int64_t sellAmount) {
        a3.changeTrust(cur3, availableBalance);
        issuer.pay(a3, cur3, availableBalance);
        a3.changeTrust(cur4, availableLimit);

        a4.changeTrust(cur4, INT64_MAX);
        issuer.pay(a4, cur4, INT64_MAX);
        a4.changeTrust(cur3, INT64_MAX);

        a4.manageOffer(0, cur4, cur3, Price{price.d, price.n},
                       offerSizeInLedger);
        auto offerID = a3.manageOffer(0, cur3, cur4, price, sellAmount);
        return std::make_tuple(
            availableBalance - checkTrustLine(a3.getPublicKey(), cur3),
            checkTrustLine(a3.getPublicKey(), cur4),
            checkOffer(a3.getPublicKey(), offerID, cur3, cur4, price));
    };

    for_current_and_previous_version_from(11, *app, [&]() {
        SECTION("with no rounding")
        {
            SECTION("sell two for five")
            {
                REQUIRE(getManageBuyOfferAmount(Price{2, 5}, 200) ==
                        getManageSellOfferAmount(Price{5, 2}, 80));
            }
            SECTION("sell one for two")
            {
                REQUIRE(getManageBuyOfferAmount(Price{1, 2}, 200) ==
                        getManageSellOfferAmount(Price{2, 1}, 100));
            }
            SECTION("sell one for one")
            {
                REQUIRE(getManageBuyOfferAmount(Price{1, 1}, 200) ==
                        getManageSellOfferAmount(Price{1, 1}, 200));
            }
            SECTION("sell two for one")
            {
                REQUIRE(getManageBuyOfferAmount(Price{2, 1}, 200) ==
                        getManageSellOfferAmount(Price{1, 2}, 400));
            }
            SECTION("sell five for two")
            {
                REQUIRE(getManageBuyOfferAmount(Price{5, 2}, 200) ==
                        getManageSellOfferAmount(Price{2, 5}, 500));
            }
        }

        SECTION("with rounding")
        {
            SECTION("sell two for five")
            {
                REQUIRE(getManageBuyOfferAmount(Price{2, 5}, 201) ==
                        getManageSellOfferAmount(Price{5, 2}, 80));
            }
            SECTION("sell one for two")
            {
                REQUIRE(getManageBuyOfferAmount(Price{1, 2}, 201) ==
                        getManageSellOfferAmount(Price{2, 1}, 100));
            }
            SECTION("sell two for one")
            {
                REQUIRE(getManageBuyOfferAmount(Price{2, 1}, 201) ==
                        getManageSellOfferAmount(Price{1, 2}, 402));
            }
            SECTION("sell five for two")
            {
                REQUIRE(getManageBuyOfferAmount(Price{5, 2}, 201) ==
                        getManageSellOfferAmount(Price{2, 5}, 503));
            }
        }
    });
}

TEST_CASE("manage buy offer matches manage sell offer when executing entirely",
          "[tx][offers]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    int64_t const txfee = app->getLedgerManager().getLastTxFee();
    int64_t const minBalancePlusFees =
        app->getLedgerManager().getLastMinBalance(0) + 100 * txfee;
    int64_t const minBalance3PlusFees =
        app->getLedgerManager().getLastMinBalance(3) + 100 * txfee;

    auto root = TestAccount::createRoot(*app);
    auto issuer = root.create("issuer", minBalancePlusFees);
    auto a1 = root.create("a1", minBalance3PlusFees);
    auto a2 = root.create("a2", minBalance3PlusFees);
    auto a3 = root.create("a3", minBalance3PlusFees);
    auto a4 = root.create("a4", minBalance3PlusFees);

    auto native = makeNativeAsset();
    auto cur1 = issuer.asset("CUR1");
    auto cur2 = issuer.asset("CUR2");
    auto cur3 = issuer.asset("CUR3");
    auto cur4 = issuer.asset("CUR4");

    int64_t const availableBalance = 100;
    int64_t const availableLimit = 100;

    auto checkOffer = [&](AccountID const& acc, int64_t offerID,
                          Asset const& selling, Asset const& buying,
                          Price const& price) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto offer = stellar::loadOffer(ltx, acc, offerID);
        auto const& oe = offer.current().data.offer();
        REQUIRE(oe.selling == selling);
        REQUIRE(oe.buying == buying);
        REQUIRE(oe.price == price);
        return oe.amount;
    };

    auto checkTrustLine = [&](AccountID const& acc, Asset const& asset) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto trust = stellar::loadTrustLine(ltx, acc, asset);
        return trust.getBalance();
    };

    auto getManageBuyOfferAmount = [&](Price const& price, int64_t buyAmount) {
        a1.changeTrust(cur1, availableBalance);
        issuer.pay(a1, cur1, availableBalance);
        a1.changeTrust(cur2, availableLimit);

        a2.changeTrust(cur2, INT64_MAX);
        issuer.pay(a2, cur2, INT64_MAX);
        a2.changeTrust(cur1, INT64_MAX);

        auto offerID = a2.manageOffer(0, cur2, cur1, price, availableBalance);
        a1.manageBuyOffer(0, cur1, cur2, price, buyAmount,
                          MANAGE_OFFER_DELETED);
        return std::make_tuple(
            availableBalance - checkTrustLine(a1.getPublicKey(), cur1),
            checkTrustLine(a1.getPublicKey(), cur2),
            checkOffer(a2.getPublicKey(), offerID, cur2, cur1, price));
    };

    auto getManageSellOfferAmount = [&](Price const& price,
                                        int64_t sellAmount) {
        a3.changeTrust(cur3, availableBalance);
        issuer.pay(a3, cur3, availableBalance);
        a3.changeTrust(cur4, availableLimit);

        a4.changeTrust(cur4, INT64_MAX);
        issuer.pay(a4, cur4, INT64_MAX);
        a4.changeTrust(cur3, INT64_MAX);

        auto offerID = a4.manageOffer(0, cur4, cur3, Price{price.d, price.n},
                                      availableBalance);
        a3.manageOffer(0, cur3, cur4, price, sellAmount, MANAGE_OFFER_DELETED);
        return std::make_tuple(availableBalance -
                                   checkTrustLine(a3.getPublicKey(), cur3),
                               checkTrustLine(a3.getPublicKey(), cur4),
                               checkOffer(a4.getPublicKey(), offerID, cur4,
                                          cur3, Price{price.d, price.n}));
    };

    for_current_and_previous_version_from(11, *app, [&]() {
        SECTION("with no rounding")
        {
            SECTION("sell two for five")
            {
                REQUIRE(getManageBuyOfferAmount(Price{2, 5}, 20) ==
                        getManageSellOfferAmount(Price{5, 2}, 8));
            }
            SECTION("sell one for two")
            {
                REQUIRE(getManageBuyOfferAmount(Price{1, 2}, 20) ==
                        getManageSellOfferAmount(Price{2, 1}, 10));
            }
            SECTION("sell one for one")
            {
                REQUIRE(getManageBuyOfferAmount(Price{1, 1}, 20) ==
                        getManageSellOfferAmount(Price{1, 1}, 20));
            }
            SECTION("sell two for one")
            {
                REQUIRE(getManageBuyOfferAmount(Price{2, 1}, 20) ==
                        getManageSellOfferAmount(Price{1, 2}, 40));
            }
            SECTION("sell five for two")
            {
                REQUIRE(getManageBuyOfferAmount(Price{5, 2}, 20) ==
                        getManageSellOfferAmount(Price{2, 5}, 50));
            }
        }

        SECTION("with rounding")
        {
            SECTION("sell two for five")
            {
                REQUIRE(getManageBuyOfferAmount(Price{2, 5}, 21) ==
                        getManageSellOfferAmount(Price{5, 2}, 8));
            }
            SECTION("sell one for two")
            {
                REQUIRE(getManageBuyOfferAmount(Price{1, 2}, 21) ==
                        getManageSellOfferAmount(Price{2, 1}, 10));
            }
            SECTION("sell two for one")
            {
                REQUIRE(getManageBuyOfferAmount(Price{2, 1}, 21) ==
                        getManageSellOfferAmount(Price{1, 2}, 42));
            }
            SECTION("sell five for two")
            {
                REQUIRE(getManageBuyOfferAmount(Price{5, 2}, 21) ==
                        getManageSellOfferAmount(Price{2, 5}, 53));
            }
        }
    });
}

TEST_CASE("manage buy offer with zero liabilities", "[tx][offers]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    int64_t const txfee = app->getLedgerManager().getLastTxFee();
    int64_t const minBalancePlusFees =
        app->getLedgerManager().getLastMinBalance(0) + 100 * txfee;
    int64_t const minBalance3PlusFees =
        app->getLedgerManager().getLastMinBalance(3) + 100 * txfee;

    auto root = TestAccount::createRoot(*app);
    auto issuer = root.create("issuer", minBalancePlusFees);
    auto a1 = root.create("a1", minBalance3PlusFees);
    auto a2 = root.create("a2", minBalance3PlusFees);

    auto native = makeNativeAsset();
    auto cur1 = issuer.asset("CUR1");
    auto cur2 = issuer.asset("CUR2");

    a1.changeTrust(cur1, 100);
    issuer.pay(a1, cur1, 100);
    a1.changeTrust(cur2, 100);

    a2.changeTrust(cur2, 100);
    issuer.pay(a2, cur2, 100);
    a2.changeTrust(cur1, 100);

    auto checkTrustLine = [&](AccountID const& acc, Asset const& asset) {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto trust = stellar::loadTrustLine(ltx, acc, asset);
        return trust.getBalance();
    };

    for_current_and_previous_version_from(11, *app, [&]() {
        SECTION("offer initially had zero liabilities and does not execute")
        {
            a1.manageBuyOffer(0, cur1, cur2, Price{1, 3}, 1,
                              MANAGE_OFFER_DELETED);
            REQUIRE(checkTrustLine(a1.getPublicKey(), cur1) == 100);
            REQUIRE(checkTrustLine(a1.getPublicKey(), cur2) == 0);
        }

        SECTION("offer had zero liabilities after executing partially")
        {
            a2.manageBuyOffer(0, cur2, cur1, Price{3, 1}, 1);
            a1.manageBuyOffer(0, cur1, cur2, Price{1, 3}, 4,
                              MANAGE_OFFER_DELETED);
            REQUIRE(checkTrustLine(a1.getPublicKey(), cur1) == 99);
            REQUIRE(checkTrustLine(a1.getPublicKey(), cur2) == 3);
        }
    });
}

TEST_CASE("manage buy offer releases liabilities before modify", "[tx][offers]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    int64_t const txfee = app->getLedgerManager().getLastTxFee();
    int64_t const minBalancePlusFees =
        app->getLedgerManager().getLastMinBalance(0) + 100 * txfee;
    int64_t const minBalance3PlusFees =
        app->getLedgerManager().getLastMinBalance(3) + 100 * txfee;

    auto root = TestAccount::createRoot(*app);
    auto issuer = root.create("issuer", minBalancePlusFees);
    auto a1 = root.create("a1", minBalance3PlusFees);

    auto native = makeNativeAsset();
    auto cur1 = issuer.asset("CUR1");
    auto cur2 = issuer.asset("CUR2");

    a1.changeTrust(cur1, 100);
    issuer.pay(a1, cur1, 100);
    a1.changeTrust(cur2, 200);

    for_current_and_previous_version_from(11, *app, [&]() {
        SECTION("change amount")
        {
            REQUIRE(a1.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 50) == 1);
            REQUIRE(a1.manageBuyOffer(1, cur1, cur2, Price{1, 1}, 100,
                                      MANAGE_OFFER_UPDATED) == 1);
            REQUIRE_THROWS_AS(
                a1.manageBuyOffer(1, cur1, cur2, Price{1, 1}, 101),
                ex_MANAGE_BUY_OFFER_UNDERFUNDED);
        }

        SECTION("change price")
        {
            REQUIRE(a1.manageBuyOffer(0, cur1, cur2, Price{1, 1}, 100) == 1);
            REQUIRE(a1.manageBuyOffer(1, cur1, cur2, Price{1, 2}, 100,
                                      MANAGE_OFFER_UPDATED) == 1);
            REQUIRE_THROWS_AS(
                a1.manageBuyOffer(1, cur1, cur2, Price{201, 100}, 100),
                ex_MANAGE_BUY_OFFER_UNDERFUNDED);
        }
    });
}
