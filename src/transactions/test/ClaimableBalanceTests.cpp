// Copyright 2020 Stellar Development Foundation and contributors. Licensed
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
#include "util/Math.h"
#include <fmt/format.h>

#include "transactions/TransactionUtils.h"

using namespace stellar;
using namespace stellar::txtest;

static Claimant
makeClaimant(AccountID const& account, ClaimPredicate const& pred)
{
    Claimant c;
    c.v0().destination = account;
    c.v0().predicate = pred;

    return c;
}

static ClaimPredicate
makeSimplePredicate(uint32_t levels)
{
    ClaimPredicate pred;
    if (levels == 0)
    {
        pred.type(CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME).absBefore() = INT64_MAX;
        return pred;
    }

    auto& orPred = pred.type(CLAIM_PREDICATE_OR).orPredicates();
    auto nextLevel = makeSimplePredicate(levels - 1);
    orPred.emplace_back(nextLevel);
    orPred.emplace_back(nextLevel);

    return pred;
}

static void
randomizePredicatePos(ClaimPredicate pred1, ClaimPredicate pred2,
                      xdr::xvector<ClaimPredicate, 2>& vec)
{
    std::uniform_int_distribution<size_t> dist(0, 1);
    bool randBool = dist(gRandomEngine);

    auto const& firstPred = randBool ? pred1 : pred2;
    auto const& secondPred = randBool ? pred2 : pred1;

    vec.emplace_back(firstPred);
    vec.emplace_back(secondPred);
}

static ClaimPredicate
makePredicate(ClaimPredicateType type, bool satisfied, TimePoint nextCloseTime,
              uint32_t secondsToNextClose)
{
    ClaimPredicate pred;
    pred.type(type);
    switch (type)
    {
    case ClaimPredicateType::CLAIM_PREDICATE_UNCONDITIONAL:
        break;
    case ClaimPredicateType::CLAIM_PREDICATE_AND:
    {
        auto pred1 =
            makePredicate(CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME, satisfied,
                          nextCloseTime, secondsToNextClose);
        auto pred2 = makePredicate(CLAIM_PREDICATE_BEFORE_RELATIVE_TIME, true,
                                   nextCloseTime, secondsToNextClose);

        randomizePredicatePos(pred1, pred2, pred.andPredicates());
        break;
    }

    case ClaimPredicateType::CLAIM_PREDICATE_OR:
    {
        auto pred1 =
            makePredicate(CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME, satisfied,
                          nextCloseTime, secondsToNextClose);
        auto pred2 = makePredicate(CLAIM_PREDICATE_BEFORE_RELATIVE_TIME, false,
                                   nextCloseTime, secondsToNextClose);

        randomizePredicatePos(pred1, pred2, pred.orPredicates());
        break;
    }

    case ClaimPredicateType::CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME:
        pred.absBefore() = satisfied ? nextCloseTime + 1 : nextCloseTime;
        break;
    case ClaimPredicateType::CLAIM_PREDICATE_AFTER_ABSOLUTE_TIME:
        pred.absAfter() = satisfied ? nextCloseTime : nextCloseTime + 1;
        break;
    case ClaimPredicateType::CLAIM_PREDICATE_BEFORE_RELATIVE_TIME:
        pred.relBefore() =
            satisfied ? secondsToNextClose + 1 : secondsToNextClose;
        break;
    case ClaimPredicateType::CLAIM_PREDICATE_AFTER_RELATIVE_TIME:
        pred.relAfter() =
            satisfied ? secondsToNextClose : secondsToNextClose + 1;
        break;
    default:
        abort();
    }

    return pred;
}

static ClaimPredicate
connectPredicate(bool isAnd, ClaimPredicate left, ClaimPredicate right)
{
    ClaimPredicate pred;
    if (isAnd)
    {
        auto& andPred = pred.type(CLAIM_PREDICATE_AND).andPredicates();
        andPred.emplace_back(left);
        andPred.emplace_back(right);
    }
    else
    {
        auto& orPred = pred.type(CLAIM_PREDICATE_OR).orPredicates();
        orPred.emplace_back(left);
        orPred.emplace_back(right);
    }

    return pred;
}

static void
validateBalancesOnCreateAndClaim(TestAccount& createAcc, TestAccount& claimAcc,
                                 Asset const& asset, int64_t amount,
                                 xdr::xvector<Claimant, 10> const& claimants,
                                 TestApplication& app,
                                 bool mergeCreateAcc = false)
{

    auto const& lm = app.getLedgerManager();

    bool const isNative = asset.type() == ASSET_TYPE_NATIVE;

    auto getAccAssetBalance = [&](TestAccount const& acc) {
        return isNative ? acc.getBalance() : acc.loadTrustLine(asset).balance;
    };

    // verify the delta in balances
    auto createAccNativeBeforeCreate = createAcc.getBalance();
    auto claimAccBalanceBeforeCreate = getAccAssetBalance(claimAcc);

    auto createAccAssetBeforeCreate = getAccAssetBalance(createAcc);
    auto balanceID = createAcc.createClaimableBalance(asset, amount, claimants);

    // if the asset is non-native, then the createAcc has two balances that
    // changed (native for reserve and non-native for amount). Check
    // non-native here
    REQUIRE((isNative || createAccAssetBeforeCreate - amount ==
                             getAccAssetBalance(createAcc)));

    // move close time forward
    closeLedgerOn(app, lm.getLastClosedLedgerNum() + 1, 2, 1, 2016);

    auto createAccNativeAfterCreate = createAcc.getBalance();
    auto claimAccBalanceAfterCreate = getAccAssetBalance(claimAcc);

    int64_t reserve = claimants.size() * lm.getLastReserve();
    REQUIRE(createAccNativeBeforeCreate - (isNative ? amount : 0) - reserve -
                static_cast<int64_t>(lm.getLastTxFee()) ==
            createAccNativeAfterCreate);

    // the claim account doesn't change on the create
    REQUIRE(claimAccBalanceBeforeCreate == claimAccBalanceAfterCreate);

    if (mergeCreateAcc)
    {
        auto root = TestAccount::createRoot(app);
        createAcc.merge(root);
    }

    claimAcc.claimClaimableBalance(balanceID);

    if (!mergeCreateAcc)
    {
        // check that reserve is sent back to createAcc
        auto createAccNativeAfterClaim = createAcc.getBalance();
        REQUIRE(createAccNativeAfterCreate + reserve ==
                createAccNativeAfterClaim);
    }

    int64_t fee = isNative ? lm.getLastTxFee() : 0;
    reserve = mergeCreateAcc ? reserve : 0;

    auto claimAccBalanceAfterClaim = getAccAssetBalance(claimAcc);
    REQUIRE(claimAccBalanceAfterCreate + amount - fee + reserve ==
            claimAccBalanceAfterClaim);
}

TEST_CASE("claimableBalance", "[tx][managedata]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto const& lm = app->getLedgerManager();

    int64_t const trustLineLimit = INT64_MAX;
    int64_t const minBalance1 = lm.getLastMinBalance(1);
    int64_t const minBalance3 = lm.getLastMinBalance(3);

    auto acc1 = root.create("acc1", minBalance3);
    auto acc2 = root.create("acc2", minBalance3);

    auto issuer = root.create("issuer", minBalance3);
    auto usd = makeAsset(issuer, "USD");
    auto native = makeNativeAsset();

    // acc2 is authorized
    acc2.changeTrust(usd, trustLineLimit);

    auto simplePred = makeSimplePredicate(3); // validPredicate

    xdr::xvector<Claimant, 10> validClaimants{makeClaimant(acc2, simplePred)};

    SECTION("not supported before version 14")
    {
        for_versions_to(13, *app, [&] {
            REQUIRE_THROWS_AS(
                acc1.createClaimableBalance(native, 100, validClaimants),
                ex_opNOT_SUPPORTED);

            REQUIRE_THROWS_AS(acc1.claimClaimableBalance(ClaimableBalanceID{}),
                              ex_opNOT_SUPPORTED);
        });
    }
    // generalize for native vs non-native assets
    for_versions_from(14, *app, [&] {
        auto balanceTestByAsset = [&](Asset const& asset, int64_t amount) {
            bool const isNative = asset.type() == ASSET_TYPE_NATIVE;

            auto fundForClaimableBalance = [&]() {
                // provide enough balance to acc1 to pay amount on the claimable
                // balance
                if (!isNative)
                {
                    issuer.pay(acc1, usd, amount);
                }
                else
                {
                    root.pay(acc1, amount);
                }
            };

            // close ledger to change closeTime from 0 so we can test predicates
            closeLedgerOn(*app, lm.getLastClosedLedgerNum() + 1, 1, 1, 2016);

            // authorize trustline
            if (!isNative)
            {
                REQUIRE_THROWS_AS(
                    acc1.createClaimableBalance(asset, amount, validClaimants),
                    ex_CREATE_CLAIMABLE_BALANCE_NO_TRUST);
            }

            // create and modify trustlines regardless of asset type so the
            // number of subentries are the same same in both scenarios.
            // create unauthorized trustline
            issuer.setOptions(
                setFlags(static_cast<uint32_t>(AUTH_REQUIRED_FLAG)));
            acc1.changeTrust(usd, trustLineLimit);

            if (!isNative)
            {
                REQUIRE_THROWS_AS(
                    acc1.createClaimableBalance(asset, amount, validClaimants),
                    ex_CREATE_CLAIMABLE_BALANCE_NOT_AUTHORIZED);
            }

            issuer.allowTrust(usd, acc1);

            REQUIRE_THROWS_AS(
                acc1.createClaimableBalance(asset, amount, validClaimants),
                ex_CREATE_CLAIMABLE_BALANCE_UNDERFUNDED);

            // Include second claimant. An additional baseReserve is
            // required, but will not be available. Account was created with
            // 3 baseReserves, but we need 4 (1 for account, 1 for
            // trustline, and 2 for claimable balance entry)

            REQUIRE_THROWS_AS(
                acc1.createClaimableBalance(asset, amount,
                                            {makeClaimant(acc2, simplePred),
                                             makeClaimant(issuer, simplePred)}),
                ex_CREATE_CLAIMABLE_BALANCE_LOW_RESERVE);

            fundForClaimableBalance();

            SECTION("valid predicate and claimant combinations")
            {
                Claimant c;
                c.v0().destination = acc2;

                for (uint32_t i = 0; i < 4; i++)
                {
                    SECTION(fmt::format("predicate at level {}", i))
                    {
                        acc2.claimClaimableBalance(acc1.createClaimableBalance(
                            asset, amount,
                            {makeClaimant(acc2, makeSimplePredicate(i))}));
                    }
                }

                for (size_t i = 1; i <= 10; ++i)
                {
                    auto pred = makeSimplePredicate(3);

                    SECTION(fmt::format("number of claimants={}", i))
                    {
                        // make sure we have enough to pay reserve. Account
                        // should already have enough for 1 claimant, so no need
                        // to add in that case
                        if (i > 1)
                        {
                            root.pay(acc1, lm.getLastReserve() * (i - 1));
                        }

                        c.v0().predicate = pred;
                        xdr::xvector<Claimant, 10> claimants{c};

                        // i - 1 because the first claimant is already in
                        // claimants
                        std::generate_n(
                            std::back_inserter(claimants), i - 1,
                            [&claimants, &pred] {
                                Claimant c;
                                c.v0().destination =
                                    getAccount(std::to_string(claimants.size()))
                                        .getPublicKey();
                                c.v0().predicate = pred;
                                return c;
                            });

                        validateBalancesOnCreateAndClaim(
                            acc1, acc2, asset, amount, claimants, *app);
                    }
                }
            }

            SECTION("validity checks")
            {
                SECTION("invalid amount")
                {
                    REQUIRE_THROWS_AS(
                        acc1.createClaimableBalance(asset, 0, validClaimants),
                        ex_CREATE_CLAIMABLE_BALANCE_MALFORMED);
                    REQUIRE_THROWS_AS(
                        acc1.createClaimableBalance(asset, -1, validClaimants),
                        ex_CREATE_CLAIMABLE_BALANCE_MALFORMED);
                }
                SECTION("invalid claimants")
                {
                    SECTION("empty claimants")
                    {
                        REQUIRE_THROWS_AS(
                            acc1.createClaimableBalance(asset, amount, {}),
                            ex_CREATE_CLAIMABLE_BALANCE_MALFORMED);
                    }
                    SECTION("duplicate claimants")
                    {
                        validClaimants.push_back(validClaimants.back());
                        REQUIRE_THROWS_AS(
                            acc1.createClaimableBalance(asset, amount,
                                                        validClaimants),
                            ex_CREATE_CLAIMABLE_BALANCE_MALFORMED);
                    }
                    SECTION("invalid predicate")
                    {
                        auto invalidPredicateTest =
                            [&](ClaimPredicate const& pred) {
                                validClaimants.back().v0().predicate = pred;
                                REQUIRE_THROWS_AS(
                                    acc1.createClaimableBalance(asset, amount,
                                                                validClaimants),
                                    ex_CREATE_CLAIMABLE_BALANCE_MALFORMED);
                            };

                        SECTION("invalid andPredicate size")
                        {
                            ClaimPredicate pred;
                            pred.type(CLAIM_PREDICATE_AND)
                                .andPredicates()
                                .emplace_back(makeSimplePredicate(1));
                            invalidPredicateTest(pred);
                        }
                        SECTION("invalid orPredicate size")
                        {
                            ClaimPredicate pred;
                            pred.type(CLAIM_PREDICATE_OR)
                                .orPredicates()
                                .emplace_back(makeSimplePredicate(1));
                            invalidPredicateTest(pred);
                        }
                        SECTION("invalid absBefore")
                        {
                            ClaimPredicate pred;
                            pred.type(CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME)
                                .absBefore() = -1;
                            invalidPredicateTest(pred);
                        }
                        SECTION("invalid absAfter")
                        {
                            ClaimPredicate pred;
                            pred.type(CLAIM_PREDICATE_AFTER_ABSOLUTE_TIME)
                                .absAfter() = -1;
                            invalidPredicateTest(pred);
                        }
                        SECTION("invalid relBefore")
                        {
                            ClaimPredicate pred;
                            pred.type(CLAIM_PREDICATE_BEFORE_RELATIVE_TIME)
                                .relBefore() = -1;
                            invalidPredicateTest(pred);
                        }
                        SECTION("invalid relAfter")
                        {
                            ClaimPredicate pred;
                            pred.type(CLAIM_PREDICATE_AFTER_RELATIVE_TIME)
                                .relAfter() = -1;
                            invalidPredicateTest(pred);
                        }
                        SECTION("invalid predicate height")
                        {
                            invalidPredicateTest(makeSimplePredicate(4));
                        }
                    }
                }
            }

            SECTION("claim balance")
            {
                uint32_t dayInSeconds = 86400;
                TimePoint nextCloseTime;
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());

                    // closeLedgerOn will move close time forward by a day
                    // (86400 seconds)
                    nextCloseTime =
                        ltx.loadHeader().current().scpValue.closeTime +
                        dayInSeconds;
                }

                auto absBeforeFalse =
                    makePredicate(CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME, false,
                                  nextCloseTime, dayInSeconds);
                auto absAfterFalse =
                    makePredicate(CLAIM_PREDICATE_AFTER_ABSOLUTE_TIME, false,
                                  nextCloseTime, dayInSeconds);
                auto relBeforeFalse =
                    makePredicate(CLAIM_PREDICATE_BEFORE_RELATIVE_TIME, false,
                                  nextCloseTime, dayInSeconds);
                auto relAfterFalse =
                    makePredicate(CLAIM_PREDICATE_AFTER_RELATIVE_TIME, false,
                                  nextCloseTime, dayInSeconds);

                auto absBeforeTrue =
                    makePredicate(CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME, true,
                                  nextCloseTime, dayInSeconds);
                auto absAfterTrue =
                    makePredicate(CLAIM_PREDICATE_AFTER_ABSOLUTE_TIME, true,
                                  nextCloseTime, dayInSeconds);
                auto relBeforeTrue =
                    makePredicate(CLAIM_PREDICATE_BEFORE_RELATIVE_TIME, true,
                                  nextCloseTime, dayInSeconds);
                auto relAfterTrue =
                    makePredicate(CLAIM_PREDICATE_AFTER_RELATIVE_TIME, true,
                                  nextCloseTime, dayInSeconds);

                SECTION("predicate not satisfied")
                {
                    auto predicateTest = [&](ClaimPredicate const& pred) {
                        auto balanceID = acc1.createClaimableBalance(
                            asset, amount, {makeClaimant(acc2, pred)});

                        // move closeTime forward by a day
                        closeLedgerOn(*app, lm.getLastClosedLedgerNum() + 1, 2,
                                      1, 2016);
                        REQUIRE_THROWS_AS(
                            acc2.claimClaimableBalance(balanceID),
                            ex_CLAIM_CLAIMABLE_BALANCE_CANNOT_CLAIM);
                    };

                    // validate 6
                    SECTION("absBefore not satisfied")
                    {
                        predicateTest(absBeforeFalse);
                    }
                    SECTION("absAfter not satisfied")
                    {
                        predicateTest(absAfterFalse);
                    }
                    SECTION("relBefore not satisfied")
                    {
                        predicateTest(relBeforeFalse);
                    }
                    SECTION("relAfter not satisfied")
                    {
                        predicateTest(relAfterFalse);
                    }
                    SECTION("relAfter max not satisfied")
                    {
                        ClaimPredicate pred;
                        pred.type(CLAIM_PREDICATE_AFTER_RELATIVE_TIME);
                        pred.relAfter() = INT64_MAX;
                        predicateTest(pred);
                    }
                    SECTION("and predicate not satisfied")
                    {
                        predicateTest(makePredicate(CLAIM_PREDICATE_AND, false,
                                                    nextCloseTime,
                                                    dayInSeconds));
                    }
                    SECTION("or predicate not satisfied")
                    {
                        predicateTest(makePredicate(CLAIM_PREDICATE_OR, false,
                                                    nextCloseTime,
                                                    dayInSeconds));
                    }
                    SECTION("complex")
                    {
                        // will fail because left will return false to the top
                        // and predicate
                        auto right =
                            connectPredicate(true, absAfterTrue, absBeforeTrue);
                        auto left = connectPredicate(false, relBeforeFalse,
                                                     absAfterFalse);
                        predicateTest(connectPredicate(true, left, right));
                    }
                    SECTION("complex 2")
                    {
                        auto right =
                            connectPredicate(true, absAfterTrue, absBeforeTrue);
                        auto left = connectPredicate(false, relBeforeFalse,
                                                     absAfterFalse);
                        auto connectedRight =
                            connectPredicate(true, left, right);

                        predicateTest(connectPredicate(false, relAfterFalse,
                                                       connectedRight));
                    }
                    SECTION("complex 3")
                    {
                        // full tree with all ORs. No predicate is satisfied
                        auto tree1 = connectPredicate(false, absAfterFalse,
                                                      absBeforeFalse);
                        auto tree2 = connectPredicate(false, relAfterFalse,
                                                      relBeforeFalse);
                        auto tree3 = connectPredicate(false, absAfterFalse,
                                                      relBeforeFalse);
                        auto tree4 = connectPredicate(false, relAfterFalse,
                                                      absBeforeFalse);

                        auto c1 = connectPredicate(false, tree1, tree2);
                        auto c2 = connectPredicate(false, tree3, tree4);
                        predicateTest(connectPredicate(false, c1, c2));
                    }
                }

                SECTION("predicate satisfied")
                {
                    auto predicateTest = [&](ClaimPredicate const& pred) {
                        auto balanceID = acc1.createClaimableBalance(
                            asset, amount, {makeClaimant(acc2, pred)});

                        // move closeTime forward by a day
                        closeLedgerOn(*app, lm.getLastClosedLedgerNum() + 1, 2,
                                      1, 2016);
                        acc2.claimClaimableBalance(balanceID);
                    };

                    SECTION("Unconditional")
                    {
                        ClaimPredicate p;
                        p.type(CLAIM_PREDICATE_UNCONDITIONAL);
                        predicateTest(p);
                    }
                    SECTION("absBefore satisfied")
                    {
                        predicateTest(absBeforeTrue);
                    }
                    SECTION("absAfter satisfied")
                    {
                        predicateTest(absAfterTrue);
                    }
                    SECTION("relBefore satisfied")
                    {
                        predicateTest(relBeforeTrue);
                    }
                    SECTION("relAfter satisfied")
                    {
                        predicateTest(relAfterTrue);
                    }
                    SECTION("relBefore max satisfied")
                    {
                        ClaimPredicate pred;
                        pred.type(CLAIM_PREDICATE_BEFORE_RELATIVE_TIME);
                        pred.relBefore() = INT64_MAX;
                        predicateTest(pred);
                    }
                    SECTION("and predicate satisfied")
                    {
                        predicateTest(makePredicate(CLAIM_PREDICATE_AND, true,
                                                    nextCloseTime,
                                                    dayInSeconds));
                    }
                    SECTION("or predicate satisfied")
                    {
                        predicateTest(makePredicate(CLAIM_PREDICATE_OR, true,
                                                    nextCloseTime,
                                                    dayInSeconds));
                    }
                    SECTION("complex 1")
                    {
                        auto right =
                            connectPredicate(true, absAfterTrue, absBeforeTrue);
                        auto left = connectPredicate(false, relBeforeTrue,
                                                     absAfterFalse);
                        predicateTest(connectPredicate(true, left, right));
                    }
                    SECTION("complex 2")
                    {
                        auto right =
                            connectPredicate(true, absAfterTrue, absBeforeTrue);
                        auto left = connectPredicate(false, relBeforeTrue,
                                                     absAfterFalse);
                        auto connectedRight =
                            connectPredicate(true, left, right);

                        predicateTest(connectPredicate(false, relAfterFalse,
                                                       connectedRight));
                    }
                    SECTION("complex 3")
                    {
                        // full tree with all ANDs. All predicates are satisfied
                        auto tree1 =
                            connectPredicate(true, absAfterTrue, absBeforeTrue);
                        auto tree2 =
                            connectPredicate(true, relAfterTrue, relBeforeTrue);
                        auto tree3 =
                            connectPredicate(true, absAfterTrue, relBeforeTrue);
                        auto tree4 =
                            connectPredicate(true, relAfterTrue, absBeforeTrue);

                        auto c1 = connectPredicate(true, tree1, tree2);
                        auto c2 = connectPredicate(true, tree3, tree4);
                        predicateTest(connectPredicate(true, c1, c2));
                    }
                    SECTION("complex 4")
                    {
                        // full tree with all ORs. One predicate is satisfied
                        auto tree1 = connectPredicate(false, absAfterFalse,
                                                      absBeforeFalse);
                        auto tree2 = connectPredicate(false, relAfterFalse,
                                                      relBeforeFalse);
                        // absAfterTrue is used here
                        auto tree3 = connectPredicate(false, absAfterTrue,
                                                      relBeforeFalse);
                        auto tree4 = connectPredicate(false, relAfterFalse,
                                                      absBeforeFalse);

                        auto c1 = connectPredicate(false, tree1, tree2);
                        auto c2 = connectPredicate(false, tree3, tree4);
                        predicateTest(connectPredicate(false, c1, c2));
                    }
                }

                SECTION("balance does not exist")
                {
                    acc1.createClaimableBalance(asset, amount, validClaimants);
                    REQUIRE_THROWS_AS(
                        acc1.claimClaimableBalance(ClaimableBalanceID{}),
                        ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);
                }

                SECTION("no destination match")
                {
                    auto simpleBalanceID = acc1.createClaimableBalance(
                        asset, amount, validClaimants);
                    // issuer is not one of the claimants
                    REQUIRE_THROWS_AS(
                        issuer.claimClaimableBalance(simpleBalanceID),
                        ex_CLAIM_CLAIMABLE_BALANCE_CANNOT_CLAIM);
                }
            }

            SECTION("multiple create and claims")
            {
                validateBalancesOnCreateAndClaim(acc1, acc2, asset, amount,
                                                 validClaimants, *app);

                fundForClaimableBalance();
                validateBalancesOnCreateAndClaim(acc1, acc2, asset, amount,
                                                 validClaimants, *app);
            }

            SECTION("balanceID relies on sequence number")
            {
                // create two claimable balances (one here and one in
                // validateBalancesOnCreateAndClaim) with the same parameters
                auto balanceID =
                    acc1.createClaimableBalance(asset, amount, validClaimants);

                // add reserve so we can create another balance entry
                root.pay(acc1, minBalance1);

                fundForClaimableBalance();
                validateBalancesOnCreateAndClaim(acc1, acc2, asset, amount,
                                                 validClaimants, *app);

                {
                    // check that the original claimable balance still exists.
                    // The balance created in validateBalancesOnCreateAndClaim
                    // is the one that was claimed
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(stellar::loadClaimableBalance(ltx, balanceID));
                }
            }

            SECTION("successful createdBy == claimant")
            {
                auto createdByIsClaimant = [&](bool createAndClaimInSameTx) {
                    auto nativeBalancePre = acc1.getBalance();
                    auto nonNativeBalancePre =
                        isNative ? 0 : acc1.loadTrustLine(asset).balance;

                    if (createAndClaimInSameTx)
                    {
                        auto id = acc1.getBalanceID(
                            0, acc1.getLastSequenceNumber() + 1);
                        auto tx =
                            acc1.tx({acc1.op(createClaimableBalance(
                                         asset, amount,
                                         {makeClaimant(acc1, simplePred)})),
                                     acc1.op(claimClaimableBalance(id))});

                        // use closeLedger so we can apply a multi op tx and get
                        // the fee charged
                        closeLedgerOn(*app, lm.getLastClosedLedgerNum() + 1, 2,
                                      1, 2016, {tx});
                    }
                    else
                    {
                        acc1.claimClaimableBalance(acc1.createClaimableBalance(
                            asset, amount, {makeClaimant(acc1, simplePred)}));
                    }

                    auto nativeBalancePost = acc1.getBalance();
                    auto nonNativeBalancePost =
                        isNative ? 0 : acc1.loadTrustLine(asset).balance;

                    int64_t feeCharged = lm.getLastTxFee() * 2;
                    REQUIRE(nativeBalancePost == nativeBalancePre - feeCharged);
                    REQUIRE(nonNativeBalancePost == nonNativeBalancePre);
                };

                SECTION("create and claim in different tx")
                {
                    createdByIsClaimant(false);
                }
                SECTION("create and claim in same tx")
                {
                    createdByIsClaimant(true);
                }
            }
        };

        SECTION("native")
        {
            int64_t const amount = lm.getLastReserve();
            balanceTestByAsset(native, amount * 2);
        }

        SECTION("non-native")
        {
            balanceTestByAsset(usd, 100);
        }

        SECTION("invalid asset")
        {
            usd.alphaNum4().assetCode[0] = 0;
            REQUIRE_THROWS_AS(
                acc1.createClaimableBalance(usd, 100, validClaimants),
                ex_CREATE_CLAIMABLE_BALANCE_MALFORMED);
        }

        SECTION("merge create account before claim")
        {
            validateBalancesOnCreateAndClaim(acc1, acc2, native, 100,
                                             validClaimants, *app, true);
        }

        SECTION("claim claimable trustline issues")
        {
            issuer.pay(acc2, usd, trustLineLimit);

            auto acc3 = root.create("acc3", minBalance3);

            auto balanceID = acc2.createClaimableBalance(
                usd, trustLineLimit - 100, {makeClaimant(acc3, simplePred)});

            REQUIRE_THROWS_AS(acc3.claimClaimableBalance(balanceID),
                              ex_CLAIM_CLAIMABLE_BALANCE_NO_TRUST);

            issuer.setOptions(
                setFlags(static_cast<uint32_t>(AUTH_REQUIRED_FLAG)));
            acc3.changeTrust(usd, trustLineLimit);

            REQUIRE_THROWS_AS(acc3.claimClaimableBalance(balanceID),
                              ex_CLAIM_CLAIMABLE_BALANCE_NOT_AUTHORIZED);

            issuer.allowTrust(usd, acc3);
            // claimable balance amount is limit - 100, so acc3 will not be able
            // to claim it with a current balance of 101
            issuer.pay(acc3, usd, 101);

            REQUIRE_THROWS_AS(acc3.claimClaimableBalance(balanceID),
                              ex_CLAIM_CLAIMABLE_BALANCE_LINE_FULL);

            // bring acc3 balance down so it doesn't exceed the trustline limit
            // on claim
            acc3.pay(issuer, usd, 1);
            acc3.claimClaimableBalance(balanceID);
        }

        SECTION("native line full")
        {
            // issuers native line is full
            issuer.manageOffer(0, usd, native, Price{1, 1},
                               INT64_MAX - issuer.getBalance());

            auto reserve = lm.getLastReserve();

            SECTION("native claim amount results in line full")
            {
                auto amount = issuer.getBalance();
                root.pay(acc2, amount);

                auto balanceId = acc2.createClaimableBalance(
                    native, amount, {makeClaimant(issuer, simplePred)});

                REQUIRE_THROWS_AS(issuer.claimClaimableBalance(balanceId),
                                  ex_CLAIM_CLAIMABLE_BALANCE_LINE_FULL);
            }

            SECTION("not enough space left for reserve to be paid back to "
                    "balance creator on claim")
            {
                SECTION("createdBy != claimant")
                {
                    auto balanceId =
                        issuer.createClaimableBalance(usd, 100, validClaimants);

                    // increase issuer native balance by reserve
                    root.pay(issuer, reserve);

                    SECTION("reserve is sent to claiming account")
                    {
                        auto issuerBalanceBeforeClaim = issuer.getBalance();
                        auto acc2BalanceBeforeClaim = acc2.getBalance();

                        acc2.claimClaimableBalance(balanceId);

                        // check that the reserve was sent to acc2 (the claiming
                        // account)
                        REQUIRE(issuerBalanceBeforeClaim ==
                                issuer.getBalance());
                        REQUIRE(acc2BalanceBeforeClaim + reserve -
                                    lm.getLastTxFee() ==
                                acc2.getBalance());
                    }

                    SECTION("use create account to claim")
                    {
                        auto issuerBalanceBeforeClaim = issuer.getBalance();
                        auto acc2BalanceBeforeClaim = acc2.getBalance();

                        acc2.claimClaimableBalance(balanceId);

                        // check that the reserve was sent to acc2 (the claiming
                        // account)
                        REQUIRE(issuerBalanceBeforeClaim ==
                                issuer.getBalance());
                        REQUIRE(acc2BalanceBeforeClaim + reserve -
                                    lm.getLastTxFee() ==
                                acc2.getBalance());
                    }

                    SECTION("no space left in both accounts")
                    {
                        auto idr = makeAsset(acc2, "IDR");

                        // acc2 native line is full
                        acc2.manageOffer(0, idr, native, Price{1, 1},
                                         INT64_MAX - acc2.getBalance());

                        REQUIRE_THROWS_AS(acc2.claimClaimableBalance(balanceId),
                                          ex_CLAIM_CLAIMABLE_BALANCE_LINE_FULL);
                    }
                }

                SECTION("createdBy == claimant")
                {
                    // issuer is the creator and claimant

                    auto balanceId = issuer.createClaimableBalance(
                        usd, 1, {makeClaimant(issuer, simplePred)});

                    // increase issuer native balance by reserve.
                    root.pay(issuer, reserve);

                    // reserve can't be paid back to the the createdBy account
                    // or the sourceAccount (both the issuer in this case)
                    REQUIRE_THROWS_AS(issuer.claimClaimableBalance(balanceId),
                                      ex_CLAIM_CLAIMABLE_BALANCE_LINE_FULL);
                }
            }
        }

        SECTION("multiple creates in tx to test index in hash")
        {
            root.pay(acc1, minBalance1);
            auto op1 = createClaimableBalance(native, 1, validClaimants);
            auto op2 = createClaimableBalance(native, 2, validClaimants);

            auto tx = acc1.tx({op1, op2});
            applyCheck(tx, *app);

            auto balanceID1 = acc1.getBalanceID(0);
            auto balanceID2 = acc1.getBalanceID(1);

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto entry1 =
                stellar::loadClaimableBalance(ltx, acc1.getBalanceID(0));
            auto entry2 =
                stellar::loadClaimableBalance(ltx, acc1.getBalanceID(1));

            REQUIRE((entry1 && entry2));

            auto validateEntry = [&](LedgerTxnEntry const& entry,
                                     ClaimableBalanceID balanceID,
                                     int64_t amount) {
                auto const& claimableBalance =
                    entry.current().data.claimableBalance();
                REQUIRE(claimableBalance.asset == native);
                REQUIRE(claimableBalance.amount == amount);
                REQUIRE(claimableBalance.balanceID == balanceID);
                REQUIRE(claimableBalance.createdBy == acc1.getPublicKey());
                REQUIRE(static_cast<uint32_t>(claimableBalance.reserve) ==
                        validClaimants.size() * lm.getLastReserve());
            };

            validateEntry(entry1, balanceID1, 1);
            validateEntry(entry2, balanceID2, 2);
        }

        SECTION("multiple claimants try to claim balance in same tx")
        {
            // add acc2 as signer on issuer account so acc2 can execute an op
            // from issuer
            auto sk1 = makeSigner(acc2, 100);
            issuer.setOptions(setSigner(sk1));

            validClaimants.emplace_back(
                makeClaimant(root, makeSimplePredicate(2)));

            // acc2 and root are claimants
            auto balanceID =
                acc1.createClaimableBalance(native, 1, validClaimants);
            auto op = claimClaimableBalance(balanceID);

            auto acc2Balance = acc2.getBalance();
            auto issuerBalance = issuer.getBalance();

            // submit tx with 2 claims
            auto tx = acc2.tx({op, issuer.op(op)});
            applyCheck(tx, *app);

            REQUIRE(tx->getResultCode() == txFAILED);
            auto const& innerRes = tx->getResult().result.results();

            REQUIRE(innerRes[0].code() == opINNER);
            REQUIRE(innerRes[1].code() == opINNER);

            // first op consumed balance entry, so second op failed
            REQUIRE(innerRes[0].tr().claimClaimableBalanceResult().code() ==
                    CLAIM_CLAIMABLE_BALANCE_SUCCESS);
            REQUIRE(innerRes[1].tr().claimClaimableBalanceResult().code() ==
                    CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);

            // only change should be the fee paid for 2 ops
            REQUIRE(acc2Balance - lm.getLastTxFee() * 2 == acc2.getBalance());
            REQUIRE(issuerBalance == issuer.getBalance());
        }

        SECTION("tx account is different than op account on successful create")
        {
            // Allow acc1 to submit an op from acc2.
            root.pay(acc2, lm.getLastMinBalance(1));
            auto sk1 = makeSigner(acc1, 100);
            acc2.setOptions(setSigner(sk1));

            applyTx(acc1.tx({acc2.op(
                        createClaimableBalance(native, 1, validClaimants))}),
                    *app);

            // second claimable balance uses acc1 for key
            auto id = acc1.getBalanceID(0);
            acc2.claimClaimableBalance(id);
        }

        SECTION("validate tx account is used in hash")
        {
            // make new accounts so sequence numbers are the same
            auto accA = root.create("accA", minBalance3);
            auto accB = root.create("accB", lm.getLastMinBalance(4));

            // Allow accA to submit an op from accB. This will bump accB's
            // seqnum up by 1
            auto sk1 = makeSigner(accA, 100);
            accB.setOptions(setSigner(sk1));

            // Move accA seqnum up by one so accA and accB have the same seqnum
            accA.bumpSequence(accB.getLastSequenceNumber());
            REQUIRE(accA.getLastSequenceNumber() ==
                    accB.getLastSequenceNumber());

            // accB and accA have the same seq num. Create a claimable balance
            // with accB twice. Once using accB as the Tx account, and once with
            // accA as the Tx account. If the claimable balance key uses the
            // operation source account and the tx source account seqnum for
            // it's hash, then the second create will fail because the keys will
            // match. We want to make sure that the tx account and the seqnum of
            // that account are used for the claimable balance key
            auto id1 = accB.createClaimableBalance(native, 100, validClaimants);
            applyTx(accA.tx({accB.op(
                        createClaimableBalance(native, 100, validClaimants))}),
                    *app);

            // second claimable balance uses accA for key
            auto id2 = accA.getBalanceID(0);

            // claim both balances
            acc2.claimClaimableBalance(id1);
            acc2.claimClaimableBalance(id2);
        }
    });
}
