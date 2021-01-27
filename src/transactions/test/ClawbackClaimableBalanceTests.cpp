// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SponsorshipTestUtils.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("clawbackClaimableBalance", "[tx][clawback][claimablebalance]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    app->start();

    auto root = TestAccount::createRoot(*app);

    auto const minBalance4 = app->getLedgerManager().getLastMinBalance(4);
    auto a1 = root.create("A1", minBalance4);
    auto gateway = root.create("gw", minBalance4);
    auto idr = makeAsset(gateway, "IDR");
    auto native = makeNativeAsset();

    ClaimPredicate u;
    u.type(CLAIM_PREDICATE_UNCONDITIONAL);

    Claimant validClaimant;
    validClaimant.v0().destination = gateway;
    validClaimant.v0().predicate = u;

    for_versions_to(15, *app, [&] {
        SECTION("pre V16 errors")
        {
            ClaimableBalanceID balanceID;
            REQUIRE_THROWS_AS(gateway.clawbackClaimableBalance(balanceID),
                              ex_opNOT_SUPPORTED);
        }
    });

    for_versions_from(16, *app, [&] {
        auto toSet = static_cast<uint32_t>(AUTH_CLAWBACK_ENABLED_FLAG |
                                           AUTH_REVOCABLE_FLAG);
        gateway.setOptions(setFlags(toSet));

        a1.changeTrust(idr, 1000);
        gateway.pay(a1, idr, 100);

        SECTION("basic test")
        {
            auto balanceID =
                a1.createClaimableBalance(idr, 99, {validClaimant});

            gateway.clawbackClaimableBalance(balanceID);

            REQUIRE_THROWS_AS(gateway.claimClaimableBalance(balanceID),
                              ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

            REQUIRE_THROWS_AS(gateway.clawbackClaimableBalance(balanceID),
                              ex_CLAWBACK_CLAIMABLE_BALANCE_DOES_NOT_EXIST);
        }

        SECTION("successful alphanum12 clawback")
        {
            auto asset12 = makeAssetAlphanum12(gateway, "DOLLAR");
            a1.changeTrust(asset12, 1000);
            gateway.pay(a1, asset12, 100);

            auto balanceID =
                a1.createClaimableBalance(asset12, 99, {validClaimant});

            gateway.clawbackClaimableBalance(balanceID);
        }

        SECTION("clawback sponsored claimable balance")
        {
            auto sponsoredClaimableBalance = [&](TestAccount& account) {
                // This scenario shouldn't be different from a normal claimable
                // balance clawback since claimable balances are not subentries,
                // and always have a "sponsoringID"

                auto tx = transactionFrameFromOps(
                    app->getNetworkID(), account,
                    {account.op(beginSponsoringFutureReserves(a1)),
                     a1.op(createClaimableBalance(idr, 100, {validClaimant})),
                     a1.op(endSponsoringFutureReserves())},
                    {a1});

                ClaimableBalanceID balanceID;
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx->apply(*app, ltx, txm));
                    REQUIRE(tx->getResultCode() == txSUCCESS);

                    // the create is the second op in the tx
                    balanceID = account.getBalanceID(1);

                    checkSponsorship(ltx, claimableBalanceKey(balanceID), 1,
                                     &account.getPublicKey());

                    // a1 has one subentry - a trustline
                    checkSponsorship(ltx, a1, 0, nullptr, 1, 0, 0, 0);
                    checkSponsorship(ltx, account, 0, nullptr, 0, 2, 1, 0);
                    ltx.commit();
                }

                gateway.clawbackClaimableBalance(balanceID);

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    checkSponsorship(ltx, account, 0, nullptr, 0, 2, 0, 0);
                }

                REQUIRE_THROWS_AS(gateway.clawbackClaimableBalance(balanceID),
                                  ex_CLAWBACK_CLAIMABLE_BALANCE_DOES_NOT_EXIST);
            };

            SECTION("sponsor is issuer")
            {
                sponsoredClaimableBalance(gateway);
            }
            SECTION("sponsor is not issuer")
            {
                sponsoredClaimableBalance(root);
            }
        }

        SECTION("issuer claimable balance")
        {
            gateway.setOptions(clearFlags(AUTH_CLAWBACK_ENABLED_FLAG));

            auto claimant = validClaimant;
            claimant.v0().destination = a1;

            auto balanceID1 =
                gateway.createClaimableBalance(idr, 100, {claimant});

            REQUIRE_THROWS_AS(
                gateway.clawbackClaimableBalance(balanceID1),
                ex_CLAWBACK_CLAIMABLE_BALANCE_NOT_CLAWBACK_ENABLED);

            gateway.setOptions(setFlags(AUTH_CLAWBACK_ENABLED_FLAG));

            auto balanceID2 =
                gateway.createClaimableBalance(idr, 100, {validClaimant});

            // issuer can clawback this balance since the issuer has
            // AUTH_CLAWBACK_ENABLED_FLAG set
            gateway.clawbackClaimableBalance(balanceID2);

            // show that an issuer claimable balance can be claimed
            a1.changeTrust(idr, 1000);
            a1.claimClaimableBalance(balanceID1);
        }

        SECTION("errors")
        {
            SECTION("not issuer")
            {
                SECTION("native")
                {
                    auto balanceID =
                        a1.createClaimableBalance(native, 1, {validClaimant});
                    REQUIRE_THROWS_AS(
                        gateway.clawbackClaimableBalance(balanceID),
                        ex_CLAWBACK_CLAIMABLE_BALANCE_NOT_ISSUER);
                }
                SECTION("assetCode4")
                {
                    auto balanceID =
                        a1.createClaimableBalance(idr, 99, {validClaimant});
                    // root is not the issuer
                    REQUIRE_THROWS_AS(root.clawbackClaimableBalance(balanceID),
                                      ex_CLAWBACK_CLAIMABLE_BALANCE_NOT_ISSUER);
                }
                SECTION("assetCode12")
                {
                    auto asset12 = makeAssetAlphanum12(gateway, "DOLLAR");
                    a1.changeTrust(asset12, 1000);
                    gateway.pay(a1, asset12, 100);

                    auto balanceID =
                        a1.createClaimableBalance(asset12, 99, {validClaimant});
                    // root is not the issuer
                    REQUIRE_THROWS_AS(root.clawbackClaimableBalance(balanceID),
                                      ex_CLAWBACK_CLAIMABLE_BALANCE_NOT_ISSUER);
                }
            }
            SECTION("not clawback enabled")
            {
                gateway.setOptions(clearFlags(AUTH_CLAWBACK_ENABLED_FLAG));

                auto usd = makeAsset(gateway, "USD");
                a1.changeTrust(usd, 1000);
                gateway.pay(a1, usd, 100);

                auto balanceID =
                    a1.createClaimableBalance(usd, 100, {validClaimant});
                REQUIRE_THROWS_AS(
                    gateway.clawbackClaimableBalance(balanceID),
                    ex_CLAWBACK_CLAIMABLE_BALANCE_NOT_CLAWBACK_ENABLED);

                gateway.claimClaimableBalance(balanceID);
            }
        }
    });
}