// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/test/SponsorshipTestUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"

using namespace stellar;
using namespace stellar::txtest;

static OperationResult
getOperationResult(TransactionTestFramePtr tx, size_t i)
{
    return tx->getResult().result.results()[i];
}

namespace stellar
{

static void
validateExpectedAccountV0Ext(uint32_t ledgerVersion, AccountEntry const& ae)
{
    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_19) &&
        hasAccountEntryExtV3(ae))
    {
        REQUIRE(ae.ext.v1().ext.v2().numSponsoring == 0);
        REQUIRE(ae.ext.v1().ext.v2().numSponsored == 0);
    }
    else
    {
        REQUIRE(ae.ext.v() == 0);
    }
}

void
checkSponsorship(AbstractLedgerTxn& ltx, LedgerKey const& lk, int leExt,
                 AccountID const* sponsoringID)
{
    auto ltxe = ltx.load(lk);
    auto const& le = ltxe.current();

    REQUIRE(le.ext.v() == leExt);
    if (leExt == 1)
    {
        REQUIRE(le.ext.v() == 1);
        if (!sponsoringID)
        {
            REQUIRE(!le.ext.v1().sponsoringID);
        }
        else
        {
            REQUIRE(le.ext.v1().sponsoringID);
            REQUIRE(*le.ext.v1().sponsoringID == *sponsoringID);
        }
    }
}

void
checkSponsorship(AbstractLedgerTxn& ltx, AccountID const& accountID,
                 SignerKey const& signerKey, int aeExt,
                 AccountID const* sponsoringID)
{
    auto ltxe = loadAccount(ltx, accountID);
    auto const& ae = ltxe.current().data.account();

    auto signerIt =
        std::find_if(ae.signers.begin(), ae.signers.end(),
                     [&](auto const& s) { return s.key == signerKey; });
    REQUIRE(signerIt != ae.signers.end());
    size_t n = signerIt - ae.signers.begin();

    switch (aeExt)
    {
    case 0:
        validateExpectedAccountV0Ext(ltx.loadHeader().current().ledgerVersion,
                                     ae);
        break;
    case 1:
        REQUIRE(ae.ext.v() == 1);
        REQUIRE(ae.ext.v1().ext.v() == 0);
        break;
    case 2:
        REQUIRE(ae.ext.v() == 1);
        REQUIRE(ae.ext.v1().ext.v() == 2);
        if (sponsoringID)
        {
            REQUIRE(ae.ext.v1().ext.v2().signerSponsoringIDs.at(n));
            REQUIRE(*ae.ext.v1().ext.v2().signerSponsoringIDs.at(n) ==
                    *sponsoringID);
        }
        else
        {
            REQUIRE(!ae.ext.v1().ext.v2().signerSponsoringIDs.at(n));
        }
        break;
    }
}

void
checkSponsorship(AbstractLedgerTxn& ltx, AccountID const& acc, int leExt,
                 AccountID const* sponsoringID, uint32_t numSubEntries,
                 int aeExt, uint32_t numSponsoring, uint32_t numSponsored)
{
    checkSponsorship(ltx, accountKey(acc), leExt, sponsoringID);

    auto le = loadAccount(ltx, acc);
    auto const& ae = le.current().data.account();
    REQUIRE(ae.numSubEntries == numSubEntries);
    switch (aeExt)
    {
    case 0:
        validateExpectedAccountV0Ext(ltx.loadHeader().current().ledgerVersion,
                                     ae);
        break;
    case 1:
        REQUIRE(ae.ext.v() == 1);
        break;
    case 2:
        REQUIRE(ae.ext.v() == 1);
        REQUIRE(ae.ext.v1().ext.v() == 2);
        REQUIRE(ae.ext.v1().ext.v2().numSponsoring == numSponsoring);
        REQUIRE(ae.ext.v1().ext.v2().numSponsored == numSponsored);
        break;
    }
}

void
createSponsoredEntryButSponsorHasInsufficientBalance(
    Application& app, TestAccount& sponsoringAcc, TestAccount& sponsoredAcc,
    Operation const& op, std::function<bool(OperationResult const&)> check)
{
    SECTION("create sponsored entry but sponsor has insufficient balance")
    {
        for_versions_from(14, app, [&] {
            auto root = TestAccount::createRoot(app);
            auto tx = transactionFrameFromOps(
                app.getNetworkID(), root,
                {sponsoringAcc.op(beginSponsoringFutureReserves(sponsoredAcc)),
                 sponsoredAcc.op(op),
                 sponsoredAcc.op(endSponsoringFutureReserves())},
                {sponsoringAcc.getSecretKey(), sponsoredAcc.getSecretKey()});

            LedgerTxn ltx(app.getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(
                tx->checkValidForTesting(app.getAppConnector(), ltx, 0, 0, 0));
            REQUIRE(!tx->apply(app.getAppConnector(), ltx, txm));
            REQUIRE(check(getOperationResult(tx, 1)));
            ltx.commit();
        });
    }
}

static uint32_t
getNumReservesRequiredForOperation(Operation const& op)
{
    if (op.body.type() == REVOKE_SPONSORSHIP &&
        op.body.revokeSponsorshipOp().type() ==
            REVOKE_SPONSORSHIP_LEDGER_ENTRY &&
        (op.body.revokeSponsorshipOp().ledgerKey().type() == ACCOUNT ||
         (op.body.revokeSponsorshipOp().ledgerKey().type() == TRUSTLINE &&
          op.body.revokeSponsorshipOp().ledgerKey().trustLine().asset.type() ==
              ASSET_TYPE_POOL_SHARE)))
    {
        return 2;
    }
    else if (op.body.type() == CREATE_ACCOUNT)
    {
        return 2;
    }
    else if (op.body.type() == CHANGE_TRUST &&
             op.body.changeTrustOp().line.type() == ASSET_TYPE_POOL_SHARE)
    {
        return 2;
    }
    else if (op.body.type() == CREATE_CLAIMABLE_BALANCE)
    {
        return static_cast<uint32_t>(
            op.body.createClaimableBalanceOp().claimants.size());
    }

    return 1;
}

static void
createModifyAndRemoveSponsoredEntry(Application& app, TestAccount& sponsoredAcc,
                                    Operation const& opCreate,
                                    Operation const& opModify1,
                                    Operation const& opModify2,
                                    Operation const& opRemove,
                                    RevokeSponsorshipOp const& rso,
                                    uint32_t ledgerVersionFrom)
{
    SECTION("create, modify, and remove sponsored entry")
    {
        for_versions_from(ledgerVersionFrom, app, [&] {
            uint32_t nse;
            {
                LedgerTxn ltx(app.getLedgerTxnRoot());
                auto ltxe = loadAccount(ltx, sponsoredAcc);
                nse = ltxe.current().data.account().numSubEntries;
            }

            int64_t txfee = app.getLedgerManager().getLastTxFee();
            int64_t minBalance0 = app.getLedgerManager().getLastMinBalance(0);
            auto root = TestAccount::createRoot(app);
            auto a2 = root.create("cmarseAcc1", minBalance0 + txfee);

            auto tx = transactionFrameFromOps(
                app.getNetworkID(), root,
                {root.op(beginSponsoringFutureReserves(sponsoredAcc)),
                 sponsoredAcc.op(opCreate),
                 sponsoredAcc.op(endSponsoringFutureReserves())},
                {sponsoredAcc.getSecretKey()});
            auto tx2 = transactionFrameFromOps(app.getNetworkID(), root,
                                               {sponsoredAcc.op(opModify1)},
                                               {sponsoredAcc.getSecretKey()});
            auto tx3 = transactionFrameFromOps(
                app.getNetworkID(), root,
                {a2.op(beginSponsoringFutureReserves(sponsoredAcc)),
                 sponsoredAcc.op(opModify2),
                 sponsoredAcc.op(endSponsoringFutureReserves())},
                {a2.getSecretKey(), sponsoredAcc.getSecretKey()});
            auto tx4 = transactionFrameFromOps(app.getNetworkID(), root,
                                               {sponsoredAcc.op(opRemove)},
                                               {sponsoredAcc.getSecretKey()});

            auto check = [&](AbstractLedgerTxn& l) {
                switch (rso.type())
                {
                case REVOKE_SPONSORSHIP_LEDGER_ENTRY:
                    checkSponsorship(l, rso.ledgerKey(), 1,
                                     &root.getPublicKey());
                    break;
                case REVOKE_SPONSORSHIP_SIGNER:
                    checkSponsorship(l, rso.signer().accountID,
                                     rso.signer().signerKey, 2,
                                     &root.getPublicKey());
                    break;
                default:
                    REQUIRE(false);
                }
            };

            auto numReserves = getNumReservesRequiredForOperation(opCreate);

            {
                LedgerTxn ltx(app.getLedgerTxnRoot());
                TransactionMetaFrame txm(
                    ltx.loadHeader().current().ledgerVersion);
                REQUIRE(tx->checkValidForTesting(app.getAppConnector(), ltx, 0,
                                                 0, 0));
                REQUIRE(tx->apply(app.getAppConnector(), ltx, txm));

                check(ltx);
                checkSponsorship(ltx, sponsoredAcc, 0, nullptr,
                                 nse + numReserves, 2, 0, numReserves);
                checkSponsorship(ltx, root, 0, nullptr, 0, 2, numReserves, 0);
                ltx.commit();
            }

            // Modify sponsored entry
            {
                LedgerTxn ltx2(app.getLedgerTxnRoot());
                TransactionMetaFrame txm2(
                    ltx2.loadHeader().current().ledgerVersion);
                REQUIRE(tx2->checkValidForTesting(app.getAppConnector(), ltx2,
                                                  0, 0, 0));
                REQUIRE(tx2->apply(app.getAppConnector(), ltx2, txm2));

                check(ltx2);
                checkSponsorship(ltx2, sponsoredAcc, 0, nullptr,
                                 nse + numReserves, 2, 0, numReserves);
                checkSponsorship(ltx2, root, 0, nullptr, 0, 2, numReserves, 0);
                ltx2.commit();
            }

            // Modify sponsored entry while sponsored
            {
                LedgerTxn ltx3(app.getLedgerTxnRoot());
                TransactionMetaFrame txm3(2);
                REQUIRE(tx3->checkValidForTesting(app.getAppConnector(), ltx3,
                                                  0, 0, 0));
                REQUIRE(tx3->apply(app.getAppConnector(), ltx3, txm3));

                check(ltx3);
                checkSponsorship(ltx3, sponsoredAcc, 0, nullptr,
                                 nse + numReserves, 2, 0, numReserves);
                checkSponsorship(ltx3, root, 0, nullptr, 0, 2, numReserves, 0);
                checkSponsorship(ltx3, a2, 0, nullptr, 0, 0, 0, 0);
                ltx3.commit();
            }

            // Remove sponsored entry
            {
                LedgerTxn ltx4(app.getLedgerTxnRoot());
                TransactionMetaFrame txm4(2);
                REQUIRE(tx4->checkValidForTesting(app.getAppConnector(), ltx4,
                                                  0, 0, 0));
                REQUIRE(tx4->apply(app.getAppConnector(), ltx4, txm4));

                if (rso.type() == REVOKE_SPONSORSHIP_LEDGER_ENTRY)
                {
                    REQUIRE(!ltx4.load(rso.ledgerKey()));
                }
                checkSponsorship(ltx4, sponsoredAcc, 0, nullptr, nse, 2, 0, 0);
                checkSponsorship(ltx4, root, 0, nullptr, 0, 2, 0, 0);
                ltx4.commit();
            }
        });
    }
}

void
createModifyAndRemoveSponsoredEntry(
    Application& app, TestAccount& sponsoredAcc, Operation const& opCreate,
    Operation const& opModify1, Operation const& opModify2,
    Operation const& opRemove, LedgerKey const& lk, uint32_t ledgerVersionFrom)
{
    RevokeSponsorshipOp rso(REVOKE_SPONSORSHIP_LEDGER_ENTRY);
    rso.ledgerKey() = lk;
    createModifyAndRemoveSponsoredEntry(app, sponsoredAcc, opCreate, opModify1,
                                        opModify2, opRemove, rso,
                                        ledgerVersionFrom);
}

void
createModifyAndRemoveSponsoredEntry(Application& app, TestAccount& sponsoredAcc,
                                    Operation const& opCreate,
                                    Operation const& opModify1,
                                    Operation const& opModify2,
                                    Operation const& opRemove,
                                    SignerKey const& signerKey)
{
    RevokeSponsorshipOp rso(REVOKE_SPONSORSHIP_SIGNER);
    rso.signer().accountID = sponsoredAcc;
    rso.signer().signerKey = signerKey;
    createModifyAndRemoveSponsoredEntry(app, sponsoredAcc, opCreate, opModify1,
                                        opModify2, opRemove, rso, 14);
}

void
tooManySponsoring(Application& app, TestAccount& sponsoredAcc,
                  Operation const& successfulOp, Operation const& failOp,
                  uint32_t reservesForSuccesfulOp)
{
    tooManySponsoring(app, sponsoredAcc, sponsoredAcc, successfulOp, failOp,
                      reservesForSuccesfulOp);
}

static uint32_t
getMinProtocolVersionForTooManyTestsFromOp(Operation const& op)
{
    if (op.body.type() == CHANGE_TRUST &&
        op.body.changeTrustOp().line.type() == ASSET_TYPE_POOL_SHARE)
    {
        return 18;
    }
    else if (op.body.type() == REVOKE_SPONSORSHIP &&
             op.body.revokeSponsorshipOp().type() ==
                 REVOKE_SPONSORSHIP_LEDGER_ENTRY &&
             op.body.revokeSponsorshipOp().ledgerKey().type() == TRUSTLINE &&
             op.body.revokeSponsorshipOp()
                     .ledgerKey()
                     .trustLine()
                     .asset.type() == ASSET_TYPE_POOL_SHARE)
    {
        return 18;
    }
    else if (op.body.type() == ALLOW_TRUST ||
             op.body.type() == SET_TRUST_LINE_FLAGS)
    {
        // An assumption is made here that if you are using one of these
        // operations, you are testing the pool share revoke scenario.
        return 18;
    }

    return 14;
}

static void
submitTooManySponsoringTxs(Application& app, TestAccount& successfulOpAcc,
                           TestAccount& failOpAcc,
                           Operation const& successfulOp,
                           Operation const& failOp)
{
    auto root = TestAccount::createRoot(app);
    {
        auto tx1 = transactionFrameFromOps(
            app.getNetworkID(), root,
            {root.op(beginSponsoringFutureReserves(successfulOpAcc)),
             successfulOp, successfulOpAcc.op(endSponsoringFutureReserves())},
            {successfulOpAcc});

        LedgerTxn ltx(app.getLedgerTxnRoot());
        TransactionMetaFrame txm1(ltx.loadHeader().current().ledgerVersion);
        REQUIRE(tx1->checkValidForTesting(app.getAppConnector(), ltx, 0, 0, 0));
        REQUIRE(tx1->apply(app.getAppConnector(), ltx, txm1));
        ltx.commit();
    }

    {
        auto tx2 = transactionFrameFromOps(
            app.getNetworkID(), root,
            {root.op(beginSponsoringFutureReserves(failOpAcc)), failOp,
             failOpAcc.op(endSponsoringFutureReserves())},
            {failOpAcc});

        LedgerTxn ltx(app.getLedgerTxnRoot());
        TransactionMetaFrame txm2(ltx.loadHeader().current().ledgerVersion);
        REQUIRE(tx2->checkValidForTesting(app.getAppConnector(), ltx, 0, 0, 0));
        REQUIRE(!tx2->apply(app.getAppConnector(), ltx, txm2));
        REQUIRE(tx2->getResult().result.results()[1].code() ==
                opTOO_MANY_SPONSORING);
    }
}

void
tooManySponsoring(Application& app, TestAccount& successfulOpAcc,
                  TestAccount& failOpAcc, Operation const& successfulOp,
                  Operation const& failOp, uint32_t reservesForSuccesfulOp)
{
    REQUIRE(failOp.body.type() == successfulOp.body.type());

    // root is the sponsoring account
    auto root = TestAccount::createRoot(app);
    auto minVersion = getMinProtocolVersionForTooManyTestsFromOp(successfulOp);

    auto tooManySponsoring = [&](uint32_t reservesForFirstOp) {
        SECTION("too many sponsoring")
        {
            for_versions_from(minVersion, app, [&] {
                {
                    LedgerTxn ltx(app.getLedgerTxnRoot());
                    auto acc = stellar::loadAccount(ltx, root.getPublicKey());
                    auto& le = acc.current();
                    auto& ae = le.data.account();
                    ae.ext.v(1);
                    ae.ext.v1().ext.v(2);

                    // we want to be able to do one successful op before the
                    // fail op
                    ae.ext.v1().ext.v2().numSponsoring =
                        UINT32_MAX - reservesForFirstOp;
                    ltx.commit();
                }

                submitTooManySponsoringTxs(app, successfulOpAcc, failOpAcc,
                                           successfulOp, failOp);
            });
        }
        SECTION("too many sponsoring but not due to subentries")
        {
            for_versions(minVersion, 17, app, [&] {
                {
                    LedgerTxn ltx(app.getLedgerTxnRoot());
                    auto acc = stellar::loadAccount(ltx, root.getPublicKey());
                    auto& le = acc.current();
                    auto& ae = le.data.account();
                    ae.ext.v(1);
                    ae.ext.v1().ext.v(2);

                    // we want to be able to do one successful op before the
                    // fail op
                    ae.ext.v1().ext.v2().numSponsoring =
                        UINT32_MAX - reservesForFirstOp;

                    // make sure numSubEntry + numSponsoring limit doesn't exist
                    // pre 18
                    ae.numSubEntries = 50;

                    ltx.commit();
                }

                submitTooManySponsoringTxs(app, successfulOpAcc, failOpAcc,
                                           successfulOp, failOp);
            });
        }
        SECTION("too many sponsoring but due to subentries")
        {
            for_versions_from(18, app, [&] {
                {
                    LedgerTxn ltx(app.getLedgerTxnRoot());
                    auto acc = stellar::loadAccount(ltx, root.getPublicKey());
                    auto& le = acc.current();
                    auto& ae = le.data.account();
                    ae.ext.v(1);
                    ae.ext.v1().ext.v(2);

                    // Set numSponsoring close to UINT32_MAX and set
                    // numSubEntries high enough so only the successfulOp will
                    // succeed. This should validate the numSponsoring +
                    // numSubEntries <= UINT32_MAX protocol v18 check.
                    ae.ext.v1().ext.v2().numSponsoring =
                        UINT32_MAX - reservesForFirstOp - 50;

                    ae.numSubEntries = 50;

                    ltx.commit();
                }

                submitTooManySponsoringTxs(app, successfulOpAcc, failOpAcc,
                                           successfulOp, failOp);
            });
        }
    };

    SECTION("no space left after first op")
    {
        tooManySponsoring(reservesForSuccesfulOp);
    }

    // For entries that need more than one reserve, we want to make sure the
    // second operation would still fail if there is space for one reserve
    // (instead of no space like in the test above). We assume that both
    // operations will require the same number of reserves, and if they use at
    // least two reserves each, one extra space of reserve should not make a
    // difference. This is just making sure we’re not assuming the entry
    // uses one reserve for any overflow checks.
    if (reservesForSuccesfulOp > 1)
    {
        SECTION("one reserve left after first op")
        {
            tooManySponsoring(reservesForSuccesfulOp + 1);
        }
    }
}

static void
submitTooManyNumSubEntries(Application& app, TestAccount& testAcc,
                           Operation const& successfulOp,
                           Operation const& failOp)
{
    {
        auto tx1 = transactionFrameFromOps(app.getNetworkID(), testAcc,
                                           {successfulOp}, {});

        LedgerTxn ltx(app.getLedgerTxnRoot());
        TransactionMetaFrame txm1(ltx.loadHeader().current().ledgerVersion);
        REQUIRE(tx1->checkValidForTesting(app.getAppConnector(), ltx, 0, 0, 0));
        REQUIRE(tx1->apply(app.getAppConnector(), ltx, txm1));
        ltx.commit();
    }

    {
        auto tx2 =
            transactionFrameFromOps(app.getNetworkID(), testAcc, {failOp}, {});

        LedgerTxn ltx(app.getLedgerTxnRoot());
        TransactionMetaFrame txm2(ltx.loadHeader().current().ledgerVersion);
        REQUIRE(tx2->checkValidForTesting(app.getAppConnector(), ltx, 0, 0, 0));
        REQUIRE(!tx2->apply(app.getAppConnector(), ltx, txm2));
        REQUIRE(tx2->getResult().result.results()[0].code() ==
                opTOO_MANY_SUBENTRIES);
    }
}

void
tooManySubentries(Application& app, TestAccount& testAcc,
                  Operation const& successfulOp, Operation const& failOp)
{
    REQUIRE(failOp.body.type() == successfulOp.body.type());

    // testAcc needs a high balance
    auto root = TestAccount::createRoot(app);
    root.pay(testAcc, root.getAvailableBalance() - 100);

    auto minVersion = getMinProtocolVersionForTooManyTestsFromOp(successfulOp);

    auto tooManySubentries = [&](uint32_t reservesForFirstOp) {
        SECTION("too many subentries")
        {
            for_versions_from(minVersion, app, [&] {
                {
                    LedgerTxn ltx(app.getLedgerTxnRoot());
                    auto acc =
                        stellar::loadAccount(ltx, testAcc.getPublicKey());
                    auto& le = acc.current();
                    auto& ae = le.data.account();

                    ae.numSubEntries =
                        getAccountSubEntryLimit() - reservesForFirstOp;

                    ltx.commit();
                }

                submitTooManyNumSubEntries(app, testAcc, successfulOp, failOp);
            });
        }
        SECTION("too many subentries due to numSponsoring")
        {
            for_versions_from(18, app, [&] {
                {
                    LedgerTxn ltx(app.getLedgerTxnRoot());
                    auto acc =
                        stellar::loadAccount(ltx, testAcc.getPublicKey());
                    auto& le = acc.current();
                    auto& ae = le.data.account();

                    // Set numSponsoring close to UINT32_MAX and set
                    // numSubEntries high enough so only the successfulOp will
                    // succeed. This should validate the numSponsoring +
                    // numSubEntries <= UINT32_MAX protocol v18 check.
                    ae.ext.v(1);
                    ae.ext.v1().ext.v(2);
                    ae.ext.v1().ext.v2().numSponsoring =
                        UINT32_MAX - reservesForFirstOp - 50;

                    ae.numSubEntries = 50;

                    ltx.commit();
                }

                submitTooManyNumSubEntries(app, testAcc, successfulOp, failOp);
            });
        }
    };

    auto reservesForSuccesfulOp =
        getNumReservesRequiredForOperation(successfulOp);

    SECTION("no space left after first op")
    {
        tooManySubentries(reservesForSuccesfulOp);
    }

    // See comment at the bottom of tooManySponsoring for an explanation on how
    // this works
    if (reservesForSuccesfulOp > 1)
    {
        SECTION("one reserve left after first op")
        {
            tooManySubentries(reservesForSuccesfulOp + 1);
        }
    }
}
}
