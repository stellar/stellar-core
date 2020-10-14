// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SignerKey.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SponsorshipTestUtils.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Timer.h"
#include "util/XDROperators.h"

#include <algorithm>

using namespace stellar;
using namespace stellar::txtest;

// Try setting each option to make sure it works
// try setting all at once
// try setting high threshold ones without the correct sigs
// make sure it doesn't allow us to add signers when we don't have the
// minbalance
TEST_CASE("set options", "[tx][setoptions]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto a1 =
        root.create("A", app->getLedgerManager().getLastMinBalance(0) + 1000);

    SECTION("Signers")
    {
        auto s1 = getAccount("S1");
        auto sk1 = makeSigner(s1, 1); // low right account
        auto th = setMasterWeight(100) | setLowThreshold(1) |
                  setMedThreshold(10) | setHighThreshold(100);

        SECTION("insufficient balance")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(th | setSigner(sk1)),
                                  ex_SET_OPTIONS_LOW_RESERVE);
            });
        }

        SECTION("add signer with native selling liabilities")
        {
            auto const minBal2 = app->getLedgerManager().getLastMinBalance(2);
            auto txfee = app->getLedgerManager().getLastTxFee();
            auto const native = makeNativeAsset();
            auto acc1 = root.create("acc1", minBal2 + 2 * txfee + 500 - 1);
            TestMarket market(*app);

            auto cur1 = acc1.asset("CUR1");
            market.requireChangesWithOffer({}, [&] {
                return market.addOffer(acc1, {native, cur1, Price{1, 1}, 500});
            });

            for_versions_to(9, *app,
                            [&] { acc1.setOptions(th | setSigner(sk1)); });
            for_versions_from(10, *app, [&] {
                REQUIRE_THROWS_AS(acc1.setOptions(th | setSigner(sk1)),
                                  ex_SET_OPTIONS_LOW_RESERVE);
                root.pay(acc1, txfee + 1);
                acc1.setOptions(th | setSigner(sk1));
            });
        }

        SECTION("add signer with native buying liabilities")
        {
            auto const minBal2 = app->getLedgerManager().getLastMinBalance(2);
            auto txfee = app->getLedgerManager().getLastTxFee();
            auto const native = makeNativeAsset();
            auto acc1 = root.create("acc1", minBal2 + 2 * txfee + 500 - 1);
            TestMarket market(*app);

            auto cur1 = acc1.asset("CUR1");
            market.requireChangesWithOffer({}, [&] {
                return market.addOffer(acc1, {cur1, native, Price{1, 1}, 500});
            });

            for_all_versions(*app,
                             [&] { acc1.setOptions(th | setSigner(sk1)); });
        }

        SECTION("can't use master key as alternate signer")
        {
            auto sk = makeSigner(a1, 100);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(setSigner(sk)),
                                  ex_SET_OPTIONS_BAD_SIGNER);
            });
        }

        SECTION("bad weight for master key")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(setMasterWeight(256)),
                                  ex_SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);
            });
        }
        SECTION("bad thresholds")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(setLowThreshold(256)),
                                  ex_SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);

                REQUIRE_THROWS_AS(a1.setOptions(setMedThreshold(256)),
                                  ex_SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);

                REQUIRE_THROWS_AS(a1.setOptions(setHighThreshold(256)),
                                  ex_SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);
            });
        }

        SECTION("invalid signer weight")
        {
            root.pay(a1, app->getLedgerManager().getLastMinBalance(2));

            auto sk1_over = makeSigner(s1, 256);
            for_versions_to(9, *app,
                            [&] { a1.setOptions(setSigner(sk1_over)); });
            for_versions_from(10, *app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(setSigner(sk1_over)),
                                  ex_SET_OPTIONS_BAD_SIGNER);
            });
        }

        SECTION("non-account signers")
        {
            auto countSubEntriesAndSigners = [&](uint32_t expected) {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto a1Account = stellar::loadAccount(ltx, a1);
                auto const& ae = a1Account.current().data.account();
                REQUIRE(ae.numSubEntries == expected);
                REQUIRE(ae.signers.size() == expected);
            };
            auto checkFirstSigner = [&](Signer const& sk) {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto a1Account = stellar::loadAccount(ltx, a1);
                auto const& ae = a1Account.current().data.account();
                REQUIRE(ae.signers.size() >= 1);
                REQUIRE(ae.signers[0].key == sk.key);
                REQUIRE(ae.signers[0].weight == sk.weight);
            };

            for_versions_to(2, *app, [&] {
                // add some funds
                root.pay(a1, app->getLedgerManager().getLastMinBalance(2));

                a1.setOptions(th | setSigner(sk1));

                countSubEntriesAndSigners(1);
                checkFirstSigner(sk1);

                // add signer 2
                auto s2 = getAccount("S2");
                auto sk2 = makeSigner(s2, 100);
                a1.setOptions(setSigner(sk2));

                countSubEntriesAndSigners(2);

                // add signer 3 - non account, will fail for old ledger
                SignerKey s3;
                s3.type(SIGNER_KEY_TYPE_PRE_AUTH_TX);
                Signer sk3(s3, 100);
                REQUIRE_THROWS_AS(a1.setOptions(setSigner(sk3)),
                                  ex_SET_OPTIONS_BAD_SIGNER);

                countSubEntriesAndSigners(2);

                // update signer 2
                sk2.weight = 11;
                a1.setOptions(setSigner(sk2));

                // update signer 1
                sk1.weight = 11;
                a1.setOptions(setSigner(sk1));

                // remove signer 1
                sk1.weight = 0;
                a1.setOptions(setSigner(sk1));

                countSubEntriesAndSigners(1);
                checkFirstSigner(sk2);

                // remove signer 3 - non account, not added, because of old
                // ledger
                sk3.weight = 0;
                REQUIRE_THROWS_AS(a1.setOptions(setSigner(sk3)),
                                  ex_SET_OPTIONS_BAD_SIGNER);

                countSubEntriesAndSigners(1);

                // remove signer 2
                sk2.weight = 0;
                a1.setOptions(setSigner(sk2));

                countSubEntriesAndSigners(0);
            });

            for_versions_from(3, *app, [&] {
                // add some funds
                root.pay(a1, app->getLedgerManager().getLastMinBalance(2));
                a1.setOptions(th | setSigner(sk1));

                countSubEntriesAndSigners(1);
                checkFirstSigner(sk1);

                // add signer 2
                auto s2 = getAccount("S2");
                auto sk2 = makeSigner(s2, 100);
                a1.setOptions(setSigner(sk2));

                countSubEntriesAndSigners(2);

                // add signer 3 - non account
                SignerKey s3;
                s3.type(SIGNER_KEY_TYPE_PRE_AUTH_TX);
                Signer sk3(s3, 100);
                a1.setOptions(setSigner(sk3));

                countSubEntriesAndSigners(3);

                // update signer 2
                sk2.weight = 11;
                a1.setOptions(setSigner(sk2));

                // update signer 1
                sk1.weight = 11;
                a1.setOptions(setSigner(sk1));

                // remove signer 1
                sk1.weight = 0;
                a1.setOptions(setSigner(sk1));

                countSubEntriesAndSigners(2);
                checkFirstSigner(sk2);

                // remove signer 3 - non account
                sk3.weight = 0;
                a1.setOptions(setSigner(sk3));

                countSubEntriesAndSigners(1);

                // remove signer 2
                sk2.weight = 0;
                a1.setOptions(setSigner(sk2));

                countSubEntriesAndSigners(0);
            });
        }

        SECTION("too many signers")
        {
            for_all_versions(*app, [&]() {
                for (size_t i = 0; i <= MAX_SIGNERS; ++i)
                {
                    Signer signer;
                    {
                        auto sk = SecretKey::pseudoRandomForTesting();
                        auto const& pk = sk.getPublicKey();
                        signer.key = KeyUtils::convertKey<SignerKey>(pk);
                        signer.weight = 1;
                    }

                    if (i < MAX_SIGNERS)
                    {
                        root.setOptions(setSigner(signer));
                    }
                    else
                    {
                        REQUIRE_THROWS_AS(root.setOptions(setSigner(signer)),
                                          ex_SET_OPTIONS_TOO_MANY_SIGNERS);
                    }
                }
            });
        }

        SECTION("sponsorship")
        {
            auto const minBalance0 =
                app->getLedgerManager().getLastMinBalance(0);
            auto const minBalance1 =
                app->getLedgerManager().getLastMinBalance(1);
            auto acc1 = root.create("a1", minBalance1 - 1);
            auto acc2 = root.create("a2", minBalance0);
            createSponsoredEntryButSponsorHasInsufficientBalance(
                *app, acc1, acc2, setOptions(setSigner(sk1)),
                [](OperationResult const& opRes) {
                    return opRes.tr().setOptionsResult().code() ==
                           SET_OPTIONS_LOW_RESERVE;
                });

            auto sk1b = makeSigner(s1, 2);
            createModifyAndRemoveSponsoredEntry(
                *app, acc2, setOptions(setSigner(sk1)),
                setOptions(setSigner(sk1b)), setOptions(setSigner(sk1)),
                setOptions(setSigner(makeSigner(s1, 0))), sk1.key);
        }

        SECTION("too many sponsoring")
        {
            auto signer1 = makeSigner(getAccount("S1"), 1);
            auto signer2 = makeSigner(getAccount("S2"), 1);

            tooManySponsoring(*app, a1, a1.op(setOptions(setSigner(signer1))),
                              a1.op(setOptions(setSigner(signer2))));
        }

        SECTION("delete signer that does not exist with sponsorships")
        {
            for_versions_from(14, *app, [&]() {
                auto s2 = getAccount("S2");
                auto s3 = getAccount("S3");

                auto const minBalance1 =
                    app->getLedgerManager().getLastMinBalance(1);
                auto acc1 = root.create("a1", minBalance1);

                AccountEntryExtensionV2 extV2;
                {
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {root.op(beginSponsoringFutureReserves(acc1)),
                         acc1.op(setOptions(setSigner(makeSigner(s1, 1)))),
                         acc1.op(endSponsoringFutureReserves()),
                         acc1.op(setOptions(setSigner(makeSigner(s2, 1))))},
                        {acc1.getSecretKey()});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx->apply(*app, ltx, txm));

                    checkSponsorship(ltx, acc1.getPublicKey(), 0, nullptr, 2, 2,
                                     0, 1);
                    auto ltxe = stellar::loadAccount(ltx, acc1.getPublicKey());
                    auto const& ae = ltxe.current().data.account();
                    extV2 = ae.ext.v1().ext.v2();

                    REQUIRE(ae.signers.size() == 2);
                    REQUIRE(extV2.signerSponsoringIDs.size() == 2);
                    if (makeSigner(s1, 1).key < makeSigner(s2, 1).key)
                    {
                        REQUIRE(ae.signers[0] == makeSigner(s1, 1));
                        REQUIRE(ae.signers[1] == makeSigner(s2, 1));
                        REQUIRE(*extV2.signerSponsoringIDs[0] ==
                                root.getPublicKey());
                        REQUIRE(!extV2.signerSponsoringIDs[1]);
                    }
                    else
                    {
                        REQUIRE(ae.signers[0] == makeSigner(s2, 1));
                        REQUIRE(ae.signers[1] == makeSigner(s1, 1));
                        REQUIRE(!extV2.signerSponsoringIDs[0]);
                        REQUIRE(*extV2.signerSponsoringIDs[1] ==
                                root.getPublicKey());
                    }

                    ltx.commit();
                }

                {
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), root,
                        {acc1.op(setOptions(setSigner(makeSigner(s3, 0))))},
                        {acc1.getSecretKey()});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx->apply(*app, ltx, txm));

                    checkSponsorship(ltx, acc1.getPublicKey(), 0, nullptr, 2, 2,
                                     0, 1);
                    auto ltxe = stellar::loadAccount(ltx, acc1.getPublicKey());
                    REQUIRE(extV2 ==
                            ltxe.current().data.account().ext.v1().ext.v2());
                    ltx.commit();
                }
            });
        }

        SECTION("add and remove many signers, some with sponsorships")
        {
            auto const minBal1 = app->getLedgerManager().getLastMinBalance(1);

            typedef std::pair<Signer, std::shared_ptr<TestAccount>>
                SignerAndSponsor;
            std::vector<SignerAndSponsor> signers;

            auto checkSigners = [&]() {
                auto sortedSigners = signers;
                std::sort(sortedSigners.begin(), sortedSigners.end(),
                          [](auto const& lhs, auto const& rhs) {
                              return lhs.first.key < rhs.first.key;
                          });

                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto rootAcc = stellar::loadAccount(ltx, root.getPublicKey());
                auto const& ae = rootAcc.current().data.account();

                REQUIRE(ae.signers.size() == sortedSigners.size());
                if (ae.ext.v() == 1 && ae.ext.v1().ext.v() == 2)
                {
                    auto const& extV2 = ae.ext.v1().ext.v2();
                    REQUIRE(extV2.signerSponsoringIDs.size() ==
                            sortedSigners.size());
                    for (size_t i = 0; i < sortedSigners.size(); ++i)
                    {
                        REQUIRE(ae.signers[i] == sortedSigners[i].first);
                        if (extV2.signerSponsoringIDs[i])
                        {
                            REQUIRE(sortedSigners[i].second);
                            REQUIRE(*extV2.signerSponsoringIDs[i] ==
                                    sortedSigners[i].second->getPublicKey());
                        }
                        else
                        {
                            REQUIRE(!sortedSigners[i].second);
                        }
                    }
                }
                else
                {
                    REQUIRE(
                        std::all_of(sortedSigners.begin(), sortedSigners.end(),
                                    [](auto const& s) { return !s.second; }));
                }
            };

            auto addSigner = [&]() {
                Signer signer;
                {
                    auto sk = SecretKey::pseudoRandomForTesting();
                    auto const& pk = sk.getPublicKey();
                    signer.key = KeyUtils::convertKey<SignerKey>(pk);
                    signer.weight = 1;
                }

                std::vector<Operation> ops;
                std::vector<SecretKey> keys;
                ops.emplace_back(root.op(setOptions(setSigner(signer))));

                std::uniform_int_distribution<size_t> dist(0, 1);
                if (dist(gRandomEngine))
                {
                    auto sk = SecretKey::pseudoRandomForTesting();
                    keys.emplace_back(sk);

                    auto sponsor =
                        std::make_shared<TestAccount>(root.create(sk, minBal1));
                    signers.push_back({signer, sponsor});

                    ops.insert(
                        ops.begin(),
                        sponsor->op(beginSponsoringFutureReserves(root)));
                    ops.emplace_back(root.op(endSponsoringFutureReserves()));
                }
                else
                {
                    signers.push_back({signer, nullptr});
                }

                auto tx = transactionFrameFromOps(app->getNetworkID(), root,
                                                  ops, keys);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                TransactionMeta txm(2);
                REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                REQUIRE(tx->apply(*app, ltx, txm));
                ltx.commit();

                checkSigners();
            };

            auto removeSigner = [&]() {
                REQUIRE(!signers.empty());
                auto signer = signers.back().first;
                signer.weight = 0;
                root.setOptions(setSigner(signer));

                signers.pop_back();
                checkSigners();
            };

            for_versions_from(14, *app, [&]() {
                std::uniform_int_distribution<size_t> dist(0, 2);

                // 67% change to add, 33% chance to remove
                while (signers.size() < MAX_SIGNERS)
                {
                    if (dist(gRandomEngine))
                    {
                        addSigner();
                    }
                    else if (!signers.empty())
                    {
                        removeSigner();
                    }
                }

                // 33% change to add, 67% chance to remove
                while (!signers.empty())
                {
                    if (dist(gRandomEngine))
                    {
                        removeSigner();
                    }
                    else if (signers.size() < MAX_SIGNERS)
                    {
                        addSigner();
                    }
                }
            });
        }
    }

    SECTION("flags")
    {
        SECTION("Can't set and clear same flag")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(setFlags(AUTH_REQUIRED_FLAG) |
                                                clearFlags(AUTH_REQUIRED_FLAG)),
                                  ex_SET_OPTIONS_BAD_FLAGS);
            });
        }
        SECTION("auth flags")
        {
            for_all_versions(*app, [&] {
                a1.setOptions(setFlags(AUTH_REQUIRED_FLAG));
                a1.setOptions(setFlags(AUTH_REVOCABLE_FLAG));
                a1.setOptions(clearFlags(AUTH_REVOCABLE_FLAG));
                a1.setOptions(setFlags(AUTH_IMMUTABLE_FLAG));

                // at this point trying to change any flag should fail
                REQUIRE_THROWS_AS(
                    a1.setOptions(clearFlags(AUTH_IMMUTABLE_FLAG)),
                    ex_SET_OPTIONS_CANT_CHANGE);
                REQUIRE_THROWS_AS(a1.setOptions(clearFlags(AUTH_REQUIRED_FLAG)),
                                  ex_SET_OPTIONS_CANT_CHANGE);
                REQUIRE_THROWS_AS(a1.setOptions(setFlags(AUTH_REVOCABLE_FLAG)),
                                  ex_SET_OPTIONS_CANT_CHANGE);
            });
        }
    }

    SECTION("Home domain")
    {
        SECTION("invalid home domain")
        {
            for_all_versions(*app, [&] {
                std::string bad[] = {"abc\r", "abc\x7F",
                                     std::string("ab\000c", 4)};
                for (auto& s : bad)
                {
                    REQUIRE_THROWS_AS(a1.setOptions(setHomeDomain(s)),
                                      ex_SET_OPTIONS_INVALID_HOME_DOMAIN);
                }
            });
        }
    }

    // these are all tested by other tests
    // set InflationDest
    // set flags
    // set transfer rate
    // set data
    // set thresholds
    // set signer
}
