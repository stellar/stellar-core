// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "invariant/AccountSubEntriesCountIsValid.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "invariant/InvariantTestUtils.h"
#include "ledger/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include <random>
#include <xdrpp/autocheck.h>

using namespace stellar;
using namespace stellar::InvariantTestUtils;

static LedgerEntry
generateRandomAccountWithNoSubEntries(uint32_t ledgerSeq)
{
    LedgerEntry le;
    le.lastModifiedLedgerSeq = ledgerSeq;
    le.data.type(ACCOUNT);
    le.data.account() = LedgerTestUtils::generateValidAccountEntry(5);

    le.data.account().signers.clear();
    le.data.account().numSubEntries = 0;
    return le;
}

static LedgerEntry
generateRandomSubEntry(LedgerEntry const& acc)
{
    static auto validAccountIDGenerator =
        autocheck::map([](AccountID&& id, size_t s) { return id; },
                       autocheck::generator<AccountID>());
    static auto validDataNameGenerator = autocheck::map(
        [](string64&& dn, size_t s) {
            LedgerTestUtils::replaceControlCharacters(dn, 1);
            return dn;
        },
        autocheck::generator<string64>());

    LedgerEntry le;
    do
    {
        le = LedgerTestUtils::generateValidLedgerEntry(5);
    } while (le.data.type() == ACCOUNT);
    le.lastModifiedLedgerSeq = acc.lastModifiedLedgerSeq;

    switch (le.data.type())
    {
    case OFFER:
        le.data.offer().sellerID = acc.data.account().accountID;
        break;
    case TRUSTLINE:
        le.data.trustLine().accountID = acc.data.account().accountID;
        le.data.trustLine().asset.alphaNum4().issuer =
            validAccountIDGenerator();
        break;
    case DATA:
        le.data.data().accountID = acc.data.account().accountID;
        le.data.data().dataName = validDataNameGenerator(64);
        break;
    case ACCOUNT:
    default:
        abort();
    }
    return le;
}

static LedgerEntry
generateRandomModifiedSubEntry(LedgerEntry const& acc, LedgerEntry const& se)
{
    LedgerEntry res;
    do
    {
        res = generateRandomSubEntry(acc);
    } while (res.data.type() != se.data.type());

    switch (se.data.type())
    {
    case ACCOUNT:
        break;
    case OFFER:
        res.data.offer().offerID = se.data.offer().offerID;
        break;
    case TRUSTLINE:
        res.data.trustLine().accountID = se.data.trustLine().accountID;
        res.data.trustLine().asset = se.data.trustLine().asset;
        break;
    case DATA:
        res.data.data().dataName = se.data.data().dataName;
        break;
    default:
        abort();
    }
    return res;
}

static auto validSignerGenerator = autocheck::map(
    [](Signer&& signer, size_t s) {
        signer.weight = signer.weight & UINT8_MAX;
        if (signer.weight == 0)
        {
            signer.weight = 100;
        }
        return signer;
    },
    autocheck::generator<Signer>());

static void
updateAccountSubEntries(Application& app, LedgerEntry& le,
                        EntryFrame::pointer ef, int32_t deltaNumSubEntries,
                        UpdateList const& updatesBase)
{
    if (deltaNumSubEntries != 0)
    {
        soci::transaction sqlTx(app.getDatabase().getSession());
        auto updates = updatesBase;
        updates.push_back(std::make_tuple(EntryFrame::FromXDR(le), ef));
        REQUIRE(!store(app, updates));
    }
    {
        soci::transaction sqlTx(app.getDatabase().getSession());
        auto updates = updatesBase;
        le.data.account().numSubEntries += deltaNumSubEntries;
        updates.push_back(std::make_tuple(EntryFrame::FromXDR(le), ef));
        REQUIRE(store(app, updates));
        sqlTx.commit();
    }
}

static void
addRandomSubEntryToAccount(Application& app, LedgerEntry& le,
                           std::vector<EntryFrame::pointer>& subentries)
{
    auto& acc = le.data.account();

    bool addSigner = false;
    if (acc.signers.size() < acc.signers.max_size())
    {
        auto letGen = autocheck::generator<LedgerEntryType>();
        addSigner = (letGen(5) == ACCOUNT);
    }

    auto ef = EntryFrame::FromXDR(le);
    ++le.lastModifiedLedgerSeq;
    if (addSigner)
    {
        acc.signers.push_back(validSignerGenerator());
        updateAccountSubEntries(app, le, ef, 1, {});
    }
    else
    {
        auto sef = EntryFrame::FromXDR(generateRandomSubEntry(le));
        subentries.push_back(sef);
        updateAccountSubEntries(app, le, ef, 1, makeUpdateList(sef, nullptr));
    }
}

static void
modifyRandomSubEntryFromAccount(Application& app, LedgerEntry& le,
                                std::vector<EntryFrame::pointer>& subentries,
                                std::default_random_engine& gen)
{
    auto& acc = le.data.account();
    REQUIRE(le.data.account().numSubEntries > 0);

    bool modifySigner = false;
    if (acc.signers.size() > 0)
    {
        auto letGen = autocheck::generator<LedgerEntryType>();
        modifySigner = subentries.empty() || (letGen(5) == ACCOUNT);
    }

    auto ef = EntryFrame::FromXDR(le);
    ++le.lastModifiedLedgerSeq;
    if (modifySigner)
    {
        std::uniform_int_distribution<uint32_t> dist(
            0, uint32_t(acc.signers.size()) - 1);
        acc.signers.at(dist(gen)) = validSignerGenerator();
        updateAccountSubEntries(app, le, ef, 0, {});
    }
    else
    {
        std::uniform_int_distribution<uint32_t> dist(
            0, uint32_t(subentries.size()) - 1);
        auto index = dist(gen);
        auto sef = subentries.at(index);
        auto sef2 = EntryFrame::FromXDR(
            generateRandomModifiedSubEntry(le, sef->mEntry));
        subentries.at(index) = sef2;
        updateAccountSubEntries(app, le, ef, 0, makeUpdateList(sef2, sef));
    }
}

static void
deleteRandomSubEntryFromAccount(Application& app, LedgerEntry& le,
                                std::vector<EntryFrame::pointer>& subentries,
                                std::default_random_engine& gen)
{
    auto& acc = le.data.account();
    REQUIRE(le.data.account().numSubEntries > 0);

    bool deleteSigner = false;
    if (acc.signers.size() > 0)
    {
        auto letGen = autocheck::generator<LedgerEntryType>();
        deleteSigner = subentries.empty() || (letGen(5) == ACCOUNT);
    }

    auto ef = EntryFrame::FromXDR(le);
    ++le.lastModifiedLedgerSeq;
    if (deleteSigner)
    {
        std::uniform_int_distribution<uint32_t> dist(
            0, uint32_t(acc.signers.size()) - 1);
        acc.signers.erase(acc.signers.begin() + dist(gen));
        updateAccountSubEntries(app, le, ef, -1, {});
    }
    else
    {
        std::uniform_int_distribution<uint32_t> dist(
            0, uint32_t(subentries.size()) - 1);
        auto index = dist(gen);
        auto sef = subentries.at(index);
        subentries.erase(subentries.begin() + index);
        updateAccountSubEntries(app, le, ef, -1, makeUpdateList(nullptr, sef));
    }
}

TEST_CASE("Create account with no subentries",
          "[invariant][accountsubentriescount]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"AccountSubEntriesCountIsValid"};
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    for (uint32_t i = 0; i < 100; ++i)
    {
        auto le = generateRandomAccountWithNoSubEntries(2);
        {
            soci::transaction sqlTx(app->getDatabase().getSession());
            auto ef = EntryFrame::FromXDR(le);
            REQUIRE(store(*app, makeUpdateList(ef, nullptr)));
            REQUIRE(store(*app, makeUpdateList(nullptr, ef)));
        }
    }
}

TEST_CASE("Create account then add signers and subentries",
          "[invariant][accountsubentriescount]")
{
    std::default_random_engine gen;
    std::uniform_int_distribution<int32_t> changesDist(-1, 2);
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"AccountSubEntriesCountIsValid"};

    for (uint32_t i = 0; i < 50; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le = generateRandomAccountWithNoSubEntries(2);
        {
            auto ef = EntryFrame::FromXDR(le);
            REQUIRE(store(*app, makeUpdateList(ef, nullptr)));
        }

        std::vector<EntryFrame::pointer> subentries;
        for (uint32_t j = 0; j < 50; ++j)
        {
            auto change = changesDist(gen);
            if (change > 0 || le.data.account().numSubEntries == 0)
            {
                addRandomSubEntryToAccount(*app, le, subentries);
            }
            else if (change == 0)
            {
                modifyRandomSubEntryFromAccount(*app, le, subentries, gen);
            }
            else if (change < 0)
            {
                deleteRandomSubEntryFromAccount(*app, le, subentries, gen);
            }
        }

        auto ef = EntryFrame::FromXDR(le);
        if (le.data.account().numSubEntries != le.data.account().signers.size())
        {
            soci::transaction sqlTx(app->getDatabase().getSession());
            REQUIRE(!store(*app, makeUpdateList(nullptr, ef)));
        }
        {
            soci::transaction sqlTx(app->getDatabase().getSession());
            UpdateList apply(makeUpdateList(nullptr, ef));
            for (auto const& sef : subentries)
            {
                apply.push_back(std::make_tuple(nullptr, sef));
            }
            REQUIRE(store(*app, apply));
        }
    }
}
