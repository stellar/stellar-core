// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/test/InvariantTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "xdrpp/autocheck.h"

using namespace stellar;
using namespace stellar::InvariantTestUtils;

static int32_t
getNumClaimants(LedgerEntry const& le)
{
    return static_cast<int32_t>(le.data.claimableBalance().claimants.size());
}

TEST_CASE("sponsorship invariant", "[invariant][sponsorshipcountisvalid]")
{
    VirtualClock clock;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"SponsorshipCountIsValid"};
    auto app = createTestApplication(clock, cfg);

    auto createAccount = [&](AccountID const& accountID, bool reqV2 = false) {
        LedgerEntry le;
        le.data.type(ACCOUNT);
        do
        {
            le.data.account() = LedgerTestUtils::generateValidAccountEntry();
        } while (reqV2 && !hasAccountEntryExtV2(le.data.account()));
        le.data.account().accountID = accountID;
        return std::make_shared<LedgerEntry>(le);
    };

    auto clearAccountSponsorships = [&](AccountEntry& ae) {
        if (hasAccountEntryExtV2(ae))
        {
            auto& extV2 = ae.ext.v1().ext.v2();
            extV2.numSponsoring = 0;
            extV2.numSponsored = 0;
            extV2.signerSponsoringIDs.clear();
            extV2.signerSponsoringIDs.resize(
                static_cast<int32_t>(ae.signers.size()));
        }
    };

    auto createEntry = [&](LedgerEntryType let, AccountID const& accountID) {
        LedgerEntry le;
        le.data.type(let);
        switch (let)
        {
        case ACCOUNT:
            le.data.account() = LedgerTestUtils::generateValidAccountEntry();
            le.data.account().accountID = accountID;
            clearAccountSponsorships(le.data.account());
            break;
        case TRUSTLINE:
            le.data.trustLine() =
                LedgerTestUtils::generateValidTrustLineEntry();
            le.data.trustLine().accountID = accountID;
            break;
        case OFFER:
            le.data.offer() = LedgerTestUtils::generateValidOfferEntry();
            le.data.offer().sellerID = accountID;
            break;
        case DATA:
            le.data.data() = LedgerTestUtils::generateValidDataEntry();
            le.data.data().accountID = accountID;
            break;
        case CLAIMABLE_BALANCE:
            le.data.claimableBalance() =
                LedgerTestUtils::generateValidClaimableBalanceEntry();
            break;
        default:
            abort();
        }
        return std::make_shared<LedgerEntry>(le);
    };

    auto doNumSponsoring = [&](LedgerEntry const& leInit, int32_t delta,
                               bool update = true) {
        auto le = leInit;
        auto& extV2 = prepareAccountEntryExtensionV2(le.data.account());
        if (update)
        {
            extV2.numSponsoring += delta;
        }
        return std::make_shared<LedgerEntry>(le);
    };

    auto doNumSponsored = [&](LedgerEntry const& leInit, int32_t delta,
                              bool update = true) {
        auto le = leInit;
        auto& extV2 = prepareAccountEntryExtensionV2(le.data.account());
        if (update)
        {
            extV2.numSponsored += delta;
        }
        return std::make_shared<LedgerEntry>(le);
    };

    auto doSponsoring = [&](LedgerEntry const& leInit,
                            AccountID const& sponsoringID, bool update = true) {
        auto le = leInit;
        auto& extV1 = prepareLedgerEntryExtensionV1(le);
        if (update)
        {
            extV1.sponsoringID.activate() = sponsoringID;
        }
        return std::make_shared<LedgerEntry>(le);
    };

    auto doAddSigner = [&](LedgerEntry const& leInit,
                           AccountID const& sponsoringID, bool update = true) {
        auto le = leInit;
        auto& ae = le.data.account();
        auto& extV2 = prepareAccountEntryExtensionV2(ae);
        if (update)
        {
            auto signer = autocheck::generator<Signer>()(5);
            auto findRes = findSignerByKey(ae.signers.begin(), ae.signers.end(),
                                           signer.key);
            auto it = ae.signers.insert(findRes.first, signer);
            size_t n = it - ae.signers.begin();
            auto ssIt = extV2.signerSponsoringIDs.insert(
                extV2.signerSponsoringIDs.begin() + n, SponsorshipDescriptor{});
            ssIt->activate() = sponsoringID;
        }
        return std::make_shared<LedgerEntry>(le);
    };

    auto doUpdateSigner = [&](LedgerEntry const& leInit,
                              AccountID const& prevSponsoringID,
                              AccountID const& newSponsoringID) {
        auto le = leInit;
        auto& ae = le.data.account();
        auto& extV2 = prepareAccountEntryExtensionV2(ae);

        auto ssIt = std::find_if(
            extV2.signerSponsoringIDs.begin(), extV2.signerSponsoringIDs.end(),
            [&](auto const& x) { return x && *x == prevSponsoringID; });
        REQUIRE(ssIt != extV2.signerSponsoringIDs.end());
        **ssIt = newSponsoringID;
        return std::make_shared<LedgerEntry>(le);
    };

    auto prepareState = [&](LedgerTxn& ltxOuter,
                            std::vector<LedgerEntry> const& entries) {
        LedgerTxn ltx(ltxOuter);
        for (auto const& e : entries)
        {
            ltx.create(e);
        }
        ltx.commit();
    };

    auto check = [&](LedgerTxn& ltxOuter, UpdateList const& ul) {
        LedgerTxn ltx(ltxOuter);
        bool res = store(*app, ul, &ltx);
        ltx.commit();
        return res;
    };

    auto sponsoringID = autocheck::generator<AccountID>()(5);
    auto sponsoredID = autocheck::generator<AccountID>()(5);

    SECTION("create sponsored entry")
    {
        auto test = [&](bool updateNumSponsoring, bool updateNumSponsored,
                        bool updateSponsoring) {
            // ACCOUNT
            if (updateNumSponsoring || updateNumSponsored || updateSponsoring)
            {
                auto entry = createEntry(ACCOUNT, sponsoredID);
                entry = doSponsoring(*entry, sponsoringID, updateSponsoring);
                entry = doNumSponsored(*entry, 2, updateNumSponsored);
                auto initSponsoring = createAccount(sponsoringID);
                auto sponsoring =
                    doNumSponsoring(*initSponsoring, 2, updateNumSponsoring);

                LedgerTxn ltx(app->getLedgerTxnRoot());
                prepareState(ltx, {*initSponsoring});
                bool res = updateNumSponsoring && updateNumSponsored &&
                           updateSponsoring;
                REQUIRE(res == check(ltx, {{entry, nullptr},
                                           {sponsoring, initSponsoring}}));
            }

            if (updateNumSponsoring || updateNumSponsored || updateSponsoring)
            {
                for (auto let : {TRUSTLINE, OFFER, DATA})
                {
                    auto entry = createEntry(let, sponsoredID);
                    entry =
                        doSponsoring(*entry, sponsoringID, updateSponsoring);
                    auto initSponsoring = createAccount(sponsoringID);
                    auto sponsoring = doNumSponsoring(*initSponsoring, 1,
                                                      updateNumSponsoring);
                    auto initSponsored = createAccount(sponsoredID);
                    auto sponsored =
                        doNumSponsored(*initSponsored, 1, updateNumSponsored);

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    prepareState(ltx, {*initSponsoring, *initSponsored});
                    bool res = updateNumSponsoring && updateNumSponsored &&
                               updateSponsoring;
                    REQUIRE(res == check(ltx, {{entry, nullptr},
                                               {sponsoring, initSponsoring},
                                               {sponsored, initSponsored}}));
                }
            }

            // CLAIMABLE_BALANCE
            if (updateNumSponsoring || updateSponsoring)
            {
                auto entry = createEntry(CLAIMABLE_BALANCE, sponsoredID);
                entry = doSponsoring(*entry, sponsoringID, updateSponsoring);
                auto initSponsoring = createAccount(sponsoringID);
                auto sponsoring =
                    doNumSponsoring(*initSponsoring, getNumClaimants(*entry),
                                    updateNumSponsoring);

                LedgerTxn ltx(app->getLedgerTxnRoot());
                prepareState(ltx, {*initSponsoring});
                bool res = updateNumSponsoring && updateSponsoring;
                REQUIRE(res == check(ltx, {{entry, nullptr},
                                           {sponsoring, initSponsoring}}));
            }
        };

        test(true, true, true);
        test(true, true, false);
        test(true, false, true);
        test(true, false, false);
        test(false, true, true);
        test(false, true, false);
        test(false, false, true);
    }

    SECTION("create sponsored signer")
    {
        auto test = [&](bool updateNumSponsoring, bool updateNumSponsored,
                        bool updateSponsoring) {
            auto initEntry = createEntry(ACCOUNT, sponsoredID);
            if (initEntry->data.account().signers.size() == MAX_SIGNERS)
            {
                initEntry->data.account().signers.pop_back();
                if (hasAccountEntryExtV2(initEntry->data.account()))
                {
                    auto& extV2 = initEntry->data.account().ext.v1().ext.v2();
                    extV2.signerSponsoringIDs.pop_back();
                }
            }
            auto entry = doNumSponsored(*initEntry, 1, updateNumSponsored);
            entry = doAddSigner(*entry, sponsoringID, updateSponsoring);
            auto initSponsoring = createAccount(sponsoringID);
            auto sponsoring =
                doNumSponsoring(*initSponsoring, 1, updateNumSponsoring);

            LedgerTxn ltx(app->getLedgerTxnRoot());
            prepareState(ltx, {*initEntry, *initSponsoring});
            bool res =
                updateNumSponsoring && updateNumSponsored && updateSponsoring;
            REQUIRE(res == check(ltx, {{entry, initEntry},
                                       {sponsoring, initSponsoring}}));
        };

        test(true, true, true);
        test(true, true, false);
        test(true, false, true);
        test(true, false, false);
        test(false, true, true);
        test(false, true, false);
        test(false, false, true);
    }

    // Test is too complicated to support variations as written, so only test
    // success cases
    SECTION("update sponsored entry")
    {
        auto sponsoringID2 = autocheck::generator<AccountID>()(5);

        // ACCOUNT
        {
            auto initEntry = createEntry(ACCOUNT, sponsoredID);
            auto entry1 = doSponsoring(*initEntry, sponsoringID);
            entry1 = doNumSponsored(*entry1, 2);
            auto entry2 = doSponsoring(*entry1, sponsoringID2);
            auto initSponsoring1 = createAccount(sponsoringID, true);
            auto sponsoring1 = doNumSponsoring(*initSponsoring1, 2);
            auto initSponsoring2 = createAccount(sponsoringID2, true);
            auto sponsoring2 = doNumSponsoring(*initSponsoring2, 2);

            LedgerTxn ltx(app->getLedgerTxnRoot());
            prepareState(ltx, {*initEntry, *initSponsoring1, *initSponsoring2});
            REQUIRE(check(
                ltx, {{entry1, initEntry}, {sponsoring1, initSponsoring1}}));
            REQUIRE(check(ltx, {{entry2, entry1},
                                {initSponsoring1, sponsoring1},
                                {sponsoring2, initSponsoring2}}));
            REQUIRE(check(
                ltx, {{initEntry, entry2}, {initSponsoring2, sponsoring2}}));
        }

        for (auto let : {TRUSTLINE, OFFER, DATA})
        {
            auto initEntry = createEntry(let, sponsoredID);
            auto entry1 = doSponsoring(*initEntry, sponsoringID);
            auto entry2 = doSponsoring(*entry1, sponsoringID2);
            auto initSponsoring1 = createAccount(sponsoringID, true);
            auto sponsoring1 = doNumSponsoring(*initSponsoring1, 1);
            auto initSponsoring2 = createAccount(sponsoringID2, true);
            auto sponsoring2 = doNumSponsoring(*initSponsoring2, 1);
            auto initSponsored = createAccount(sponsoredID, true);
            auto sponsored = doNumSponsored(*initSponsored, 1);

            LedgerTxn ltx(app->getLedgerTxnRoot());
            prepareState(ltx, {*initEntry, *initSponsoring1, *initSponsoring2,
                               *initSponsored});
            REQUIRE(check(ltx, {{entry1, initEntry},
                                {sponsoring1, initSponsoring1},
                                {sponsored, initSponsored}}));
            REQUIRE(check(ltx, {{entry2, entry1},
                                {initSponsoring1, sponsoring1},
                                {sponsoring2, initSponsoring2}}));
            REQUIRE(check(ltx, {{initEntry, entry2},
                                {initSponsoring2, sponsoring2},
                                {initSponsored, sponsored}}));
        }

        // CLAIMABLE_BALANCE
        {
            auto initEntry = createEntry(CLAIMABLE_BALANCE, sponsoredID);
            auto entry1 = doSponsoring(*initEntry, sponsoringID);
            auto entry2 = doSponsoring(*entry1, sponsoringID2);
            auto initSponsoring1 = createAccount(sponsoringID, true);
            auto sponsoring1 =
                doNumSponsoring(*initSponsoring1, getNumClaimants(*entry1));
            auto initSponsoring2 = createAccount(sponsoringID2, true);
            auto sponsoring2 =
                doNumSponsoring(*initSponsoring2, getNumClaimants(*entry2));

            LedgerTxn ltx(app->getLedgerTxnRoot());
            prepareState(ltx, {*initEntry, *initSponsoring1, *initSponsoring2});
            REQUIRE(check(
                ltx, {{entry1, initEntry}, {sponsoring1, initSponsoring1}}));
            REQUIRE(check(ltx, {{entry2, entry1},
                                {initSponsoring1, sponsoring1},
                                {sponsoring2, initSponsoring2}}));
            REQUIRE(check(
                ltx, {{initEntry, entry2}, {initSponsoring2, sponsoring2}}));
        }
    }

    // Test is too complicated to support variations as written, so only test
    // success cases
    SECTION("update sponsored signer")
    {
        auto sponsoringID2 = autocheck::generator<AccountID>()(5);

        auto initEntry = createEntry(ACCOUNT, sponsoredID);
        auto entry1 = doNumSponsored(*initEntry, 1);
        entry1 = doAddSigner(*entry1, sponsoringID);
        auto entry2 = doUpdateSigner(*entry1, sponsoringID, sponsoringID2);
        auto initSponsoring1 = createAccount(sponsoringID, true);
        auto sponsoring1 = doNumSponsoring(*initSponsoring1, 1);
        auto initSponsoring2 = createAccount(sponsoringID2, true);
        auto sponsoring2 = doNumSponsoring(*initSponsoring2, 1);

        LedgerTxn ltx(app->getLedgerTxnRoot());
        prepareState(ltx, {*initEntry, *initSponsoring1, *initSponsoring2});
        REQUIRE(
            check(ltx, {{entry1, initEntry}, {sponsoring1, initSponsoring1}}));
        REQUIRE(check(ltx, {{entry2, entry1},
                            {initSponsoring1, sponsoring1},
                            {sponsoring2, initSponsoring2}}));
        REQUIRE(
            check(ltx, {{initEntry, entry2}, {initSponsoring2, sponsoring2}}));
    }

    SECTION("erase sponsored entry")
    {
        auto test = [&](bool updateNumSponsoring, bool updateNumSponsored,
                        bool updateSponsoring) {
            // ACCOUNT
            if (updateNumSponsoring || updateNumSponsored || updateSponsoring)
            {
                auto initEntry = createEntry(ACCOUNT, sponsoredID);
                initEntry =
                    doSponsoring(*initEntry, sponsoringID, updateSponsoring);
                initEntry = doNumSponsored(*initEntry, 2, updateNumSponsored);
                auto initSponsoring = createAccount(sponsoringID, true);
                auto sponsoring =
                    doNumSponsoring(*initSponsoring, -2, updateNumSponsoring);

                LedgerTxn ltx(app->getLedgerTxnRoot());
                prepareState(ltx, {*initEntry, *initSponsoring});
                bool res = updateNumSponsoring && updateNumSponsored &&
                           updateSponsoring;
                REQUIRE(res == check(ltx, {{nullptr, initEntry},
                                           {sponsoring, initSponsoring}}));
            }

            if (updateNumSponsoring || updateNumSponsored || updateSponsoring)
            {
                for (auto let : {TRUSTLINE, OFFER, DATA})
                {
                    auto initEntry = createEntry(let, sponsoredID);
                    initEntry = doSponsoring(*initEntry, sponsoringID,
                                             updateSponsoring);
                    auto initSponsoring = createAccount(sponsoringID, true);
                    auto sponsoring = doNumSponsoring(*initSponsoring, -1,
                                                      updateNumSponsoring);
                    auto initSponsored = createAccount(sponsoredID, true);
                    auto sponsored =
                        doNumSponsored(*initSponsored, -1, updateNumSponsored);

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    prepareState(ltx,
                                 {*initEntry, *initSponsoring, *initSponsored});
                    bool res = updateNumSponsoring && updateNumSponsored &&
                               updateSponsoring;
                    REQUIRE(res == check(ltx, {{nullptr, initEntry},
                                               {sponsoring, initSponsoring},
                                               {sponsored, initSponsored}}));
                }
            }

            // CLAIMABLE_BALANCE
            if (updateNumSponsoring || updateSponsoring)
            {
                auto initEntry = createEntry(CLAIMABLE_BALANCE, sponsoredID);
                initEntry =
                    doSponsoring(*initEntry, sponsoringID, updateSponsoring);
                auto initSponsoring = createAccount(sponsoringID, true);
                auto sponsoring = doNumSponsoring(*initSponsoring,
                                                  -getNumClaimants(*initEntry),
                                                  updateNumSponsoring);

                LedgerTxn ltx(app->getLedgerTxnRoot());
                prepareState(ltx, {*initEntry, *initSponsoring});
                bool res = updateNumSponsoring && updateSponsoring;
                REQUIRE(res == check(ltx, {{nullptr, initEntry},
                                           {sponsoring, initSponsoring}}));
            }
        };

        test(true, true, true);
        test(true, true, false);
        test(true, false, true);
        test(true, false, false);
        test(false, true, true);
        test(false, true, false);
        test(false, false, true);
    }

    SECTION("erase sponsored signer")
    {
        auto test = [&](bool updateNumSponsoring, bool updateNumSponsored,
                        bool updateSponsoring) {
            auto entry = createEntry(ACCOUNT, sponsoredID);
            if (entry->data.account().signers.size() == MAX_SIGNERS)
            {
                entry->data.account().signers.pop_back();
                if (hasAccountEntryExtV2(entry->data.account()))
                {
                    auto& extV2 = entry->data.account().ext.v1().ext.v2();
                    extV2.signerSponsoringIDs.pop_back();
                }
            }
            auto initEntry = doNumSponsored(*entry, 1, updateNumSponsored);
            initEntry = doAddSigner(*initEntry, sponsoringID, updateSponsoring);
            auto sponsoring = createAccount(sponsoringID);
            auto initSponsoring =
                doNumSponsoring(*sponsoring, 1, updateNumSponsoring);

            LedgerTxn ltx(app->getLedgerTxnRoot());
            prepareState(ltx, {*initEntry, *initSponsoring});
            bool res =
                updateNumSponsoring && updateNumSponsored && updateSponsoring;
            REQUIRE(res == check(ltx, {{entry, initEntry},
                                       {sponsoring, initSponsoring}}));
        };

        test(true, true, true);
        test(true, true, false);
        test(true, false, true);
        test(true, false, false);
        test(false, true, true);
        test(false, true, false);
        test(false, false, true);
    }

    SECTION("create one, erase another")
    {
        auto entry1 = createEntry(TRUSTLINE, sponsoredID);
        entry1 = doSponsoring(*entry1, sponsoringID);
        auto initEntry2 = createEntry(OFFER, sponsoredID);
        initEntry2 = doSponsoring(*initEntry2, sponsoringID);

        LedgerTxn ltx(app->getLedgerTxnRoot());
        prepareState(ltx, {*initEntry2});
        REQUIRE(check(ltx, {{entry1, nullptr}, {nullptr, initEntry2}}));
    }
}
