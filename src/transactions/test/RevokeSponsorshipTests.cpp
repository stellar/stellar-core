// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SponsorshipTestUtils.h"
#include <fmt/format.h>

using namespace stellar;
using namespace stellar::txtest;

static RevokeSponsorshipResultCode
getRevokeSponsorshipResultCode(TransactionFrameBasePtr& tx, size_t i)
{
    auto const& opRes = tx->getResult().result.results()[i];
    return opRes.tr().revokeSponsorshipResult().code();
}

Claimant
getClaimant(TestAccount const& account)
{
    ClaimPredicate pred;
    pred.type(CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME).absBefore() = INT64_MAX;

    Claimant c;
    c.v0().destination = account;
    c.v0().predicate = pred;

    return c;
}

TEST_CASE("update sponsorship", "[tx][sponsorship]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    auto minBal = [&](uint32_t n) {
        return app->getLedgerManager().getLastMinBalance(n);
    };

    auto root = TestAccount::createRoot(*app);

    for_versions_from(14, *app, [&]() {
        SECTION("entry is not sponsored")
        {
            // No-op
            SECTION("account is not sponsored")
            {
                SECTION("account")
                {
                    auto a1 = root.create("a1", minBal(1));
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), a1,
                        {a1.op(revokeSponsorship(accountKey(a1)))}, {});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx->apply(*app, ltx, txm));

                    checkSponsorship(ltx, accountKey(a1), 0, nullptr);
                    checkSponsorship(ltx, a1, 0, nullptr, 0, 0, 0, 0);
                    ltx.commit();
                }

                SECTION("trust line")
                {
                    auto cur1 = makeAsset(root, "CUR1");
                    auto a1 = root.create("a1", minBal(2));
                    a1.changeTrust(cur1, 1000);
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), a1,
                        {a1.op(revokeSponsorship(trustlineKey(a1, cur1)))}, {});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx->apply(*app, ltx, txm));

                    checkSponsorship(ltx, trustlineKey(a1, cur1), 0, nullptr);
                    checkSponsorship(ltx, a1, 0, nullptr, 1, 0, 0, 0);
                    ltx.commit();
                }

                SECTION("signer")
                {
                    auto cur1 = makeAsset(root, "CUR1");
                    auto a1 = root.create("a1", minBal(2));
                    auto signer = makeSigner(getAccount("S1"), 1);
                    a1.setOptions(setSigner(signer));
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), a1,
                        {a1.op(revokeSponsorship(a1, signer.key))}, {});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx->apply(*app, ltx, txm));

                    checkSponsorship(ltx, a1, signer.key, 0, nullptr);
                    checkSponsorship(ltx, a1, 0, nullptr, 1, 0, 0, 0);
                    ltx.commit();
                }

                SECTION("claimable balances")
                {
                    auto native = makeNativeAsset();
                    auto a1 = root.create("a1", minBal(2));

                    auto balanceID =
                        a1.createClaimableBalance(native, 1, {getClaimant(a1)});

                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), a1,
                        {a1.op(
                            revokeSponsorship(claimableBalanceKey(balanceID)))},
                        {});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(!tx->apply(*app, ltx, txm));

                    REQUIRE(getRevokeSponsorshipResultCode(tx, 0) ==
                            REVOKE_SPONSORSHIP_ONLY_TRANSFERABLE);

                    checkSponsorship(ltx, claimableBalanceKey(balanceID), 1,
                                     &a1.getPublicKey());
                    checkSponsorship(ltx, a1, 0, nullptr, 0, 2, 1, 0);
                    ltx.commit();
                }
            }

            // Transfers sponsorship to sponsor-of-account
            SECTION("account is sponsored")
            {
                SECTION("account")
                {
                    auto a1 = root.create("a1", minBal(1));

                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), a1,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(revokeSponsorship(accountKey(a1))),
                         a1.op(endSponsoringFutureReserves())},
                        {root});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx->apply(*app, ltx, txm));

                    checkSponsorship(ltx, accountKey(a1), 1,
                                     &root.getPublicKey());
                    checkSponsorship(ltx, a1, 1, &root.getPublicKey(), 0, 2, 0,
                                     2);
                    ltx.commit();
                }
                SECTION("trust line")
                {
                    auto cur1 = makeAsset(root, "CUR1");
                    auto a1 = root.create("a1", minBal(2));
                    a1.changeTrust(cur1, 1000);
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), a1,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(revokeSponsorship(trustlineKey(a1, cur1))),
                         a1.op(endSponsoringFutureReserves())},
                        {root});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx->apply(*app, ltx, txm));

                    checkSponsorship(ltx, trustlineKey(a1, cur1), 1,
                                     &root.getPublicKey());
                    checkSponsorship(ltx, a1, 0, nullptr, 1, 2, 0, 1);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 1, 0);
                    ltx.commit();
                }

                SECTION("signer")
                {
                    auto sponsorSigner = [&](bool hasSponsoredEntry) {
                        auto cur1 = makeAsset(root, "CUR1");
                        auto a1 = root.create("a1", minBal(2));
                        auto signer = makeSigner(getAccount("S1"), 1);
                        a1.setOptions(setSigner(signer));

                        // adding a sponsored entry before sponsoring the signer
                        // will guarantee that the account is a V2 extension
                        if (hasSponsoredEntry)
                        {
                            auto tx = transactionFrameFromOps(
                                app->getNetworkID(), root,
                                {root.op(beginSponsoringFutureReserves(a1)),
                                 a1.op(changeTrust(cur1, 1000)),
                                 a1.op(endSponsoringFutureReserves())},
                                {a1});

                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            TransactionMeta txm(2);
                            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                            REQUIRE(tx->apply(*app, ltx, txm));
                            ltx.commit();
                        }

                        auto tx = transactionFrameFromOps(
                            app->getNetworkID(), a1,
                            {root.op(beginSponsoringFutureReserves(a1)),
                             a1.op(revokeSponsorship(a1, signer.key)),
                             a1.op(endSponsoringFutureReserves())},
                            {root});

                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        TransactionMeta txm(2);
                        REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                        REQUIRE(tx->apply(*app, ltx, txm));

                        checkSponsorship(ltx, a1, signer.key, 2,
                                         &root.getPublicKey());
                        if (hasSponsoredEntry)
                        {
                            checkSponsorship(ltx, a1, 0, nullptr, 2, 2, 0, 2);
                            checkSponsorship(ltx, root, 0, nullptr, 0, 2, 2, 0);
                        }
                        else
                        {
                            checkSponsorship(ltx, a1, 0, nullptr, 1, 2, 0, 1);
                            checkSponsorship(ltx, root, 0, nullptr, 0, 2, 1, 0);
                        }
                        ltx.commit();
                    };

                    SECTION("Account has sponsored entry")
                    {
                        sponsorSigner(true);
                    }
                    SECTION("Signer is the only sponsorship")
                    {
                        sponsorSigner(false);
                    }
                }

                SECTION("claimable balances")
                {
                    auto native = makeNativeAsset();
                    auto a1 = root.create("a1", minBal(2));

                    auto balanceID =
                        a1.createClaimableBalance(native, 1, {getClaimant(a1)});

                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), a1,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(
                             revokeSponsorship(claimableBalanceKey(balanceID))),
                         a1.op(endSponsoringFutureReserves())},
                        {root});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx->apply(*app, ltx, txm));

                    checkSponsorship(ltx, claimableBalanceKey(balanceID), 1,
                                     &root.getPublicKey());
                    checkSponsorship(ltx, a1, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }
            }
        }

        SECTION("entry is sponsored")
        {
            // Revokes sponsorship entirely
            SECTION("sponsor is not sponsored")
            {
                SECTION("account")
                {
                    auto key = getAccount("a1");
                    TestAccount a1(*app, key);
                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         root.op(createAccount(a1, minBal(2))),
                         a1.op(endSponsoringFutureReserves())},
                        {key});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(
                            revokeSponsorship(accountKey(a1.getPublicKey())))},
                        {});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, accountKey(a1.getPublicKey()), 1,
                                     nullptr);
                    checkSponsorship(ltx, a1, 1, nullptr, 0, 2, 0, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }

                SECTION("trust line")
                {
                    auto cur1 = makeAsset(root, "CUR1");
                    auto a1 = root.create("a1", minBal(1));

                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(changeTrust(cur1, 1000)),
                         a1.op(endSponsoringFutureReserves())},
                        {a1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(revokeSponsorship(trustlineKey(a1, cur1)))},
                        {});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, trustlineKey(a1, cur1), 1, nullptr);
                    checkSponsorship(ltx, a1, 0, nullptr, 1, 2, 0, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }

                SECTION("signer")
                {
                    auto a1 = root.create("a1", minBal(1));

                    auto signer = makeSigner(getAccount("S1"), 1);
                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(setOptions(setSigner(signer))),
                         a1.op(endSponsoringFutureReserves())},
                        {a1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(revokeSponsorship(a1, signer.key))}, {});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, a1, signer.key, 2, nullptr);
                    checkSponsorship(ltx, a1, 0, nullptr, 1, 2, 0, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }

                SECTION("claimable balance")
                {
                    auto native = makeNativeAsset();
                    auto a1 = root.create("a1", minBal(1));

                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(createClaimableBalance(native, 1,
                                                      {getClaimant(a1)})),
                         a1.op(endSponsoringFutureReserves())},
                        {a1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto balanceID = tx1->getResult()
                                         .result.results()[1]
                                         .tr()
                                         .createClaimableBalanceResult()
                                         .balanceID();

                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(
                            revokeSponsorship(claimableBalanceKey(balanceID)))},
                        {});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(!tx2->apply(*app, ltx, txm2));

                    REQUIRE(getRevokeSponsorshipResultCode(tx2, 0) ==
                            REVOKE_SPONSORSHIP_ONLY_TRANSFERABLE);

                    checkSponsorship(ltx, claimableBalanceKey(balanceID), 1,
                                     &root.getPublicKey());
                    checkSponsorship(ltx, a1, 0, nullptr, 0, 0, 0, 0);
                    ltx.commit();
                }
            }

            // Transfers sponsorship to sponsor-of-sponsor
            SECTION("sponsor is sponsored")
            {
                SECTION("account")
                {
                    auto a2 = root.create("a2", minBal(2));
                    auto key = getAccount("a1");
                    TestAccount a1(*app, key);

                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         root.op(createAccount(a1, minBal(2))),
                         a1.op(endSponsoringFutureReserves())},
                        {key});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {a2.op(beginSponsoringFutureReserves(root)),
                         root.op(
                             revokeSponsorship(accountKey(a1.getPublicKey()))),
                         root.op(endSponsoringFutureReserves())},
                        {a2});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, a1, 1, &a2.getPublicKey(), 0, 2, 0,
                                     2);
                    checkSponsorship(ltx, a2, 0, nullptr, 0, 2, 2, 0);
                    ltx.commit();
                }

                SECTION("trust line")
                {
                    auto cur1 = makeAsset(root, "CUR1");
                    auto a1 = root.create("a1", minBal(0));
                    auto a2 = root.create("a2", minBal(2));

                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(changeTrust(cur1, 1000)),
                         a1.op(endSponsoringFutureReserves())},
                        {a1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {a2.op(beginSponsoringFutureReserves(root)),
                         root.op(revokeSponsorship(trustlineKey(a1, cur1))),
                         root.op(endSponsoringFutureReserves())},
                        {a2});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, trustlineKey(a1, cur1), 1,
                                     &a2.getPublicKey());
                    checkSponsorship(ltx, a1, 0, nullptr, 1, 2, 0, 1);
                    checkSponsorship(ltx, a2, 0, nullptr, 0, 2, 1, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }

                SECTION("signer")
                {
                    auto a1 = root.create("a1", minBal(0));
                    auto a2 = root.create("a2", minBal(2));

                    auto signer = makeSigner(getAccount("S1"), 1);
                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(setOptions(setSigner(signer))),
                         a1.op(endSponsoringFutureReserves())},
                        {a1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));
                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {a2.op(beginSponsoringFutureReserves(root)),
                         root.op(revokeSponsorship(a1, signer.key)),
                         root.op(endSponsoringFutureReserves())},
                        {a2});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, a1, signer.key, 2,
                                     &a2.getPublicKey());
                    checkSponsorship(ltx, a1, 0, nullptr, 1, 2, 0, 1);
                    checkSponsorship(ltx, a2, 0, nullptr, 0, 2, 1, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }

                SECTION("claimable balances")
                {
                    auto native = makeNativeAsset();
                    auto a1 = root.create("a1", minBal(0) + 1);
                    auto a2 = root.create("a2", minBal(2));

                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(createClaimableBalance(native, 1,
                                                      {getClaimant(a1)})),
                         a1.op(endSponsoringFutureReserves())},
                        {a1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto balanceID = tx1->getResult()
                                         .result.results()[1]
                                         .tr()
                                         .createClaimableBalanceResult()
                                         .balanceID();

                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {a2.op(beginSponsoringFutureReserves(root)),
                         root.op(
                             revokeSponsorship(claimableBalanceKey(balanceID))),
                         root.op(endSponsoringFutureReserves())},
                        {a2});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, claimableBalanceKey(balanceID), 1,
                                     &a2.getPublicKey());
                    checkSponsorship(ltx, a1, 0, nullptr, 0, 0, 0, 0);
                    checkSponsorship(ltx, a2, 0, nullptr, 0, 2, 1, 0);
                    ltx.commit();
                }

                SECTION("data")
                {
                    DataValue value;
                    std::string dataName = "test";
                    auto a1 = root.create("a1", minBal(0));
                    auto a2 = root.create("a2", minBal(2));

                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(manageData(dataName, &value)),
                         a1.op(endSponsoringFutureReserves())},
                        {a1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {a2.op(beginSponsoringFutureReserves(root)),
                         root.op(revokeSponsorship(dataKey(a1, dataName))),
                         root.op(endSponsoringFutureReserves())},
                        {a2});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, dataKey(a1, dataName), 1,
                                     &a2.getPublicKey());
                    checkSponsorship(ltx, a1, 0, nullptr, 1, 2, 0, 1);
                    checkSponsorship(ltx, a2, 0, nullptr, 0, 2, 1, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }

                SECTION("offer")
                {
                    auto cur1 = makeAsset(root, "CUR1");
                    auto native = makeNativeAsset();
                    auto a1 = root.create("a1", minBal(3));
                    auto a2 = root.create("a2", minBal(2));

                    a1.changeTrust(cur1, INT64_MAX);

                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(manageOffer(0, native, cur1, Price{1, 1}, 10)),
                         a1.op(endSponsoringFutureReserves())},
                        {a1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto offerID = ltx.loadHeader().current().idPool;
                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {a2.op(beginSponsoringFutureReserves(root)),
                         root.op(revokeSponsorship(offerKey(a1, offerID))),
                         root.op(endSponsoringFutureReserves())},
                        {a2});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, offerKey(a1, offerID), 1,
                                     &a2.getPublicKey());
                    checkSponsorship(ltx, a1, 0, nullptr, 2, 2, 0, 1);
                    checkSponsorship(ltx, a2, 0, nullptr, 0, 2, 1, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }
            }

            SECTION("sponsor is sponsored by owner")
            {
                SECTION("account")
                {
                    auto key = getAccount("a1");
                    TestAccount a1(*app, key);
                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         root.op(createAccount(a1, minBal(2))),
                         a1.op(endSponsoringFutureReserves())},
                        {key});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {a1.op(beginSponsoringFutureReserves(root)),
                         root.op(
                             revokeSponsorship(accountKey(a1.getPublicKey()))),
                         root.op(endSponsoringFutureReserves())},
                        {a1});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, accountKey(a1.getPublicKey()), 1,
                                     nullptr);
                    checkSponsorship(ltx, a1, 1, nullptr, 0, 2, 0, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }

                SECTION("trustline")
                {
                    auto cur1 = makeAsset(root, "CUR1");
                    auto a1 = root.create("a1", minBal(1));

                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(changeTrust(cur1, 1000)),
                         a1.op(endSponsoringFutureReserves())},
                        {a1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {a1.op(beginSponsoringFutureReserves(root)),
                         root.op(revokeSponsorship(trustlineKey(a1, cur1))),
                         root.op(endSponsoringFutureReserves())},
                        {a1});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, trustlineKey(a1, cur1), 1, nullptr);
                    checkSponsorship(ltx, a1, 0, nullptr, 1, 2, 0, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }

                SECTION("signer")
                {
                    auto a1 = root.create("a1", minBal(1));

                    auto signer = makeSigner(getAccount("S1"), 1);
                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(setOptions(setSigner(signer))),
                         a1.op(endSponsoringFutureReserves())},
                        {a1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {a1.op(beginSponsoringFutureReserves(root)),
                         root.op(revokeSponsorship(a1, signer.key)),
                         root.op(endSponsoringFutureReserves())},
                        {a1});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, a1, signer.key, 2, nullptr);
                    checkSponsorship(ltx, a1, 0, nullptr, 1, 2, 0, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }

                SECTION("data")
                {
                    DataValue value;
                    std::string dataName = "test";
                    auto a1 = root.create("a1", minBal(1));

                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(manageData(dataName, &value)),
                         a1.op(endSponsoringFutureReserves())},
                        {a1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {a1.op(beginSponsoringFutureReserves(root)),
                         root.op(revokeSponsorship(dataKey(a1, dataName))),
                         root.op(endSponsoringFutureReserves())},
                        {a1});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, dataKey(a1, dataName), 1, nullptr);
                    checkSponsorship(ltx, a1, 0, nullptr, 1, 2, 0, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }

                SECTION("offer")
                {
                    auto cur1 = makeAsset(root, "CUR1");
                    auto native = makeNativeAsset();
                    auto a1 = root.create("a1", minBal(3));

                    a1.changeTrust(cur1, INT64_MAX);

                    auto tx1 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(a1)),
                         a1.op(manageOffer(0, native, cur1, Price{1, 1}, 10)),
                         a1.op(endSponsoringFutureReserves())},
                        {a1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx1->apply(*app, ltx, txm1));

                    auto offerID = ltx.loadHeader().current().idPool;
                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {a1.op(beginSponsoringFutureReserves(root)),
                         root.op(revokeSponsorship(offerKey(a1, offerID))),
                         root.op(endSponsoringFutureReserves())},
                        {a1});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx2->apply(*app, ltx, txm2));

                    checkSponsorship(ltx, offerKey(a1, offerID), 1, nullptr);
                    checkSponsorship(ltx, a1, 0, nullptr, 2, 2, 0, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 2, 0, 0);
                    ltx.commit();
                }
            }
        }

        SECTION("failure tests")
        {
            SECTION("does not exist")
            {
                auto a1 = root.create("a1", minBal(1));
                auto cur1 = makeAsset(root, "CUR1");

                SECTION("trustline")
                {
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), a1,
                        {a1.op(revokeSponsorship(trustlineKey(a1, cur1)))}, {});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(!tx->apply(*app, ltx, txm));

                    REQUIRE(getRevokeSponsorshipResultCode(tx, 0) ==
                            REVOKE_SPONSORSHIP_DOES_NOT_EXIST);

                    checkSponsorship(ltx, a1, 0, nullptr, 0, 0, 0, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 0, 0, 0);
                }

                SECTION("use wrong account in offer key")
                {
                    auto native = makeNativeAsset();
                    auto a2 = root.create("a2", minBal(3));

                    a2.changeTrust(cur1, INT64_MAX);

                    a2.manageOffer(0, native, cur1, Price{1, 1}, 10);

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    auto offerID = ltx.loadHeader().current().idPool;

                    // put the wrong account on the offerKey
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), a2,
                        {a2.op(revokeSponsorship(offerKey(a1, offerID)))}, {});

                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(!tx->apply(*app, ltx, txm));

                    REQUIRE(getRevokeSponsorshipResultCode(tx, 0) ==
                            REVOKE_SPONSORSHIP_DOES_NOT_EXIST);

                    checkSponsorship(ltx, a1, 0, nullptr, 0, 0, 0, 0);
                    checkSponsorship(ltx, a2, 0, nullptr, 2, 1, 0, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 0, 0, 0);
                }

                SECTION("signer")
                {
                    auto s1 = getAccount("S1");
                    auto signer = makeSigner(s1, 1);

                    // signer with unknown account
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), a1,
                        {a1.op(
                            revokeSponsorship(s1.getPublicKey(), signer.key))},
                        {});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(!tx->apply(*app, ltx, txm));

                    REQUIRE(getRevokeSponsorshipResultCode(tx, 0) ==
                            REVOKE_SPONSORSHIP_DOES_NOT_EXIST);

                    // known account, but unknown signer
                    auto tx2 = transactionFrameFromOps(
                        app->getNetworkID(), a1,
                        {a1.op(revokeSponsorship(a1, signer.key))}, {});

                    TransactionMeta txm2(2);
                    REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                    REQUIRE(!tx2->apply(*app, ltx, txm));

                    REQUIRE(getRevokeSponsorshipResultCode(tx2, 0) ==
                            REVOKE_SPONSORSHIP_DOES_NOT_EXIST);

                    checkSponsorship(ltx, a1, 0, nullptr, 0, 0, 0, 0);
                    checkSponsorship(ltx, root, 0, nullptr, 0, 0, 0, 0);
                }
            }
            SECTION("not sponsor")
            {
                auto cur1 = makeAsset(root, "CUR1");
                auto a1 = root.create("a1", minBal(2));
                auto a2 = root.create("a2", minBal(1));

                auto s1 = getAccount("S1");
                auto signer = makeSigner(s1, 1);

                auto notSponsorTest = [&](bool isSponsored, bool entryTest) {
                    if (isSponsored)
                    {
                        Operation op =
                            entryTest ? a1.op(changeTrust(cur1, 1000))
                                      : a1.op(setOptions(setSigner(signer)));

                        auto tx = transactionFrameFromOps(
                            app->getNetworkID(), root,
                            {root.op(beginSponsoringFutureReserves(a1)), op,
                             a1.op(endSponsoringFutureReserves())},
                            {a1});

                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        TransactionMeta txm1(2);
                        REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                        REQUIRE(tx->apply(*app, ltx, txm1));
                        checkSponsorship(ltx, a1, 0, &root.getPublicKey(), 1, 2,
                                         0, 1);
                        ltx.commit();
                    }
                    else
                    {
                        if (entryTest)
                        {
                            a1.changeTrust(cur1, 1000);
                        }
                        else
                        {
                            a1.setOptions(setSigner(signer));
                        }
                    }

                    Operation op =
                        entryTest
                            ? a2.op(revokeSponsorship(trustlineKey(a1, cur1)))
                            : a2.op(revokeSponsorship(a1, signer.key));

                    auto tx = transactionFrameFromOps(app->getNetworkID(), a2,
                                                      {op}, {});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm1(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(!tx->apply(*app, ltx, txm1));

                    REQUIRE(getRevokeSponsorshipResultCode(tx, 0) ==
                            REVOKE_SPONSORSHIP_NOT_SPONSOR);

                    if (isSponsored)
                    {
                        // remove sponsorship, so we can hit the NOT_SPONSOR
                        // case for a V1 ledger entry without a sponsoringID
                        Operation opRemoveSponsorship =
                            entryTest
                                ? root.op(
                                      revokeSponsorship(trustlineKey(a1, cur1)))
                                : root.op(revokeSponsorship(a1, signer.key));

                        auto tx2 =
                            transactionFrameFromOps(app->getNetworkID(), root,
                                                    {opRemoveSponsorship}, {});

                        TransactionMeta txm2(2);
                        REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                        REQUIRE(tx2->apply(*app, ltx, txm2));
                        checkSponsorship(ltx, a1, 0, nullptr, 1, 2, 0, 0);

                        auto tx3 = transactionFrameFromOps(app->getNetworkID(),
                                                           a2, {op}, {});

                        TransactionMeta txm3(2);
                        REQUIRE(tx3->checkValid(ltx, 0, 0, 0));
                        REQUIRE(!tx3->apply(*app, ltx, txm3));

                        REQUIRE(getRevokeSponsorshipResultCode(tx3, 0) ==
                                REVOKE_SPONSORSHIP_NOT_SPONSOR);
                    }
                };

                SECTION("entry is sponsored. transfer using wrong sponsoring "
                        "account")
                {
                    notSponsorTest(true, true);
                }
                SECTION("entry is not sponsored. transfer from wrong source "
                        "account")
                {
                    notSponsorTest(false, true);
                }
                SECTION("signer is sponsored. transfer using wrong sponsoring "
                        "account")
                {
                    notSponsorTest(true, false);
                }
                SECTION("signer is not sponsored. transfer from wrong source "
                        "account")
                {
                    notSponsorTest(false, false);
                }
            }
            SECTION("low reserve")
            {
                auto s1 = getAccount("S1");
                auto signer = makeSigner(s1, 1);

                auto lowReserveTest = [&](bool entryTest) {
                    SECTION("transfer sponsorship")
                    {
                        auto cur1 = makeAsset(root, "CUR1");
                        auto a1 = root.create("a1", minBal(0));
                        auto a2 = root.create("a2", minBal(0));

                        Operation middleOpTx1 =
                            entryTest ? a1.op(changeTrust(cur1, 1000))
                                      : a1.op(setOptions(setSigner(signer)));
                        auto tx1 = transactionFrameFromOps(
                            app->getNetworkID(), root,
                            {root.op(beginSponsoringFutureReserves(a1)),
                             middleOpTx1, a1.op(endSponsoringFutureReserves())},
                            {a1});

                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        TransactionMeta txm1(2);
                        REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                        REQUIRE(tx1->apply(*app, ltx, txm1));

                        Operation middleOpTx2 =
                            entryTest
                                ? root.op(
                                      revokeSponsorship(trustlineKey(a1, cur1)))
                                : root.op(revokeSponsorship(a1, signer.key));

                        auto tx2 = transactionFrameFromOps(
                            app->getNetworkID(), root,
                            {a2.op(beginSponsoringFutureReserves(root)),
                             middleOpTx2,
                             root.op(endSponsoringFutureReserves())},
                            {a2});

                        TransactionMeta txm2(2);
                        REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                        REQUIRE(!tx2->apply(*app, ltx, txm2));

                        REQUIRE(getRevokeSponsorshipResultCode(tx2, 1) ==
                                REVOKE_SPONSORSHIP_LOW_RESERVE);
                    }
                    SECTION("remove sponsorship")
                    {
                        auto cur1 = makeAsset(root, "CUR1");
                        auto a1 = root.create("a1", minBal(0));
                        auto a2 = root.create("a2", minBal(0));

                        Operation middleOpTx1 =
                            entryTest ? a1.op(changeTrust(cur1, 1000))
                                      : a1.op(setOptions(setSigner(signer)));
                        auto tx1 = transactionFrameFromOps(
                            app->getNetworkID(), root,
                            {root.op(beginSponsoringFutureReserves(a1)),
                             middleOpTx1, a1.op(endSponsoringFutureReserves())},
                            {a1});

                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        TransactionMeta txm1(2);
                        REQUIRE(tx1->checkValid(ltx, 0, 0, 0));
                        REQUIRE(tx1->apply(*app, ltx, txm1));

                        Operation opTx2 =
                            entryTest
                                ? root.op(
                                      revokeSponsorship(trustlineKey(a1, cur1)))
                                : root.op(revokeSponsorship(a1, signer.key));

                        auto tx2 = transactionFrameFromOps(app->getNetworkID(),
                                                           root, {opTx2}, {});

                        TransactionMeta txm2(2);
                        REQUIRE(tx2->checkValid(ltx, 0, 0, 0));
                        REQUIRE(!tx2->apply(*app, ltx, txm2));

                        REQUIRE(getRevokeSponsorshipResultCode(tx2, 0) ==
                                REVOKE_SPONSORSHIP_LOW_RESERVE);
                    }
                    SECTION("establish sponsorship")
                    {
                        auto cur1 = makeAsset(root, "CUR1");
                        auto a1 = root.create("a1", minBal(2));
                        auto a2 = root.create("a2", minBal(0));

                        if (entryTest)
                        {
                            a1.changeTrust(cur1, 1000);
                        }
                        else
                        {
                            a1.setOptions(setSigner(signer));
                        }

                        Operation middleOp =
                            entryTest
                                ? a1.op(
                                      revokeSponsorship(trustlineKey(a1, cur1)))
                                : a1.op(revokeSponsorship(a1, signer.key));

                        auto tx = transactionFrameFromOps(
                            app->getNetworkID(), root,
                            {a2.op(beginSponsoringFutureReserves(a1)), middleOp,
                             a1.op(endSponsoringFutureReserves())},
                            {a1, a2});

                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        TransactionMeta txm(2);
                        REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                        REQUIRE(!tx->apply(*app, ltx, txm));

                        REQUIRE(getRevokeSponsorshipResultCode(tx, 1) ==
                                REVOKE_SPONSORSHIP_LOW_RESERVE);
                    }
                };

                SECTION("entry")
                {
                    lowReserveTest(true);
                }
                SECTION("signer")
                {
                    lowReserveTest(false);
                }
            }
        }
    });

    SECTION("too many sponsoring")
    {
        auto native = makeNativeAsset();
        auto a1 = root.create("a1", minBal(3));

        SECTION("account")
        {
            auto a2 = root.create("a2", minBal(3));
            tooManySponsoring(*app, a2, a1,
                              a2.op(revokeSponsorship(accountKey(a2))),
                              a1.op(revokeSponsorship(accountKey(a1))));
        }

        SECTION("signer")
        {
            auto signer1 = makeSigner(getAccount("S1"), 1);
            auto signer2 = makeSigner(getAccount("S2"), 1);
            a1.setOptions(setSigner(signer1));
            a1.setOptions(setSigner(signer2));

            tooManySponsoring(*app, a1,
                              a1.op(revokeSponsorship(a1, signer1.key)),
                              a1.op(revokeSponsorship(a1, signer2.key)));
        }

        SECTION("trustline")
        {
            auto cur1 = makeAsset(root, "CUR1");
            auto cur2 = makeAsset(root, "CUR2");
            a1.changeTrust(cur1, 1000);
            a1.changeTrust(cur2, 1000);

            tooManySponsoring(*app, a1,
                              a1.op(revokeSponsorship(trustlineKey(a1, cur2))),
                              a1.op(revokeSponsorship(trustlineKey(a1, cur1))));
        }
        SECTION("claimable balance")
        {
            auto id1 = a1.createClaimableBalance(native, 1, {getClaimant(a1)});
            auto id2 = a1.createClaimableBalance(native, 1, {getClaimant(a1)});

            tooManySponsoring(
                *app, a1, a1.op(revokeSponsorship(claimableBalanceKey(id1))),
                a1.op(revokeSponsorship(claimableBalanceKey(id2))));
        }
    }

    SECTION("native trust line")
    {
        for_versions({14}, *app, [&]() {
            auto tx = transactionFrameFromOps(
                app->getNetworkID(), root,
                {root.op(revokeSponsorship(trustlineKey(root, Asset{})))}, {});
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
        });

        for_versions({15}, *app, [&]() {
            auto tx = transactionFrameFromOps(
                app->getNetworkID(), root,
                {root.op(revokeSponsorship(trustlineKey(root, Asset{})))}, {});
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(!tx->checkValid(ltx, 0, 0, 0));
        });
    }

    SECTION("issuer trust line")
    {
        for_versions({14}, *app, [&]() {
            auto cur1 = makeAsset(root, "CUR1");
            auto tx = transactionFrameFromOps(
                app->getNetworkID(), root,
                {root.op(revokeSponsorship(trustlineKey(root, cur1)))}, {});
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
        });

        for_versions({15}, *app, [&]() {
            auto cur1 = makeAsset(root, "CUR1");
            auto tx = transactionFrameFromOps(
                app->getNetworkID(), root,
                {root.op(revokeSponsorship(trustlineKey(root, cur1)))}, {});
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(!tx->checkValid(ltx, 0, 0, 0));
        });
    }

    SECTION("invalid input")
    {
        auto a1 = root.create("a1", minBal(5));
        auto revoke = [&](LedgerKey const& ledgerKey) {
            auto tx = transactionFrameFromOps(
                app->getNetworkID(), a1, {a1.op(revokeSponsorship(ledgerKey))},
                {});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            uint32_t ledgerVersion = ltx.loadHeader().current().ledgerVersion;

            if (ledgerVersion == 14)
            {
                TransactionMeta txm(2);
                REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                REQUIRE(!tx->apply(*app, ltx, txm));

                REQUIRE(getRevokeSponsorshipResultCode(tx, 0) ==
                        REVOKE_SPONSORSHIP_DOES_NOT_EXIST);
            }
            else
            {
                REQUIRE(!tx->checkValid(ltx, 0, 0, 0));
            }
        };

        SECTION("invalid offer and data keys")
        {
            for_versions_from(14, *app, [&]() {
                SECTION("invalid offer id")
                {
                    revoke(offerKey(a1, static_cast<uint64>(-1LL)));
                }
                SECTION("invalid data name")
                {
                    SECTION("empty data name")
                    {
                        revoke(dataKey(a1, ""));
                    }
                    SECTION("control char in data name")
                    {
                        revoke(dataKey(a1, "\n"));
                    }
                }
            });
        }

        SECTION("invalid trustline keys")
        {
            auto a2 = root.create("a2", minBal(1));
            for_versions_from(15, *app, [&]() {
                auto invalidAssets = testutil::getInvalidAssets(a1);
                for (auto const& asset : invalidAssets)
                {
                    revoke(trustlineKey(a2, asset));
                }
            });
        }
    }
}
