// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "AccountFrame.h"
#include "LedgerDelta.h"
#include "OfferFrame.h"
#include "TrustFrame.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "xdrpp/autocheck.h"
#include "xdrpp/marshal.h"
#include <memory>
#include <unordered_map>
#include <utility>

using namespace stellar;

namespace LedgerEntryTests
{

TEST_CASE("Ledger Entry tests", "[ledgerentry]")
{
    Config cfg(getTestConfig(0));

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);
    app->start();
    Database& db = app->getDatabase();

    SECTION("round trip with database")
    {
        std::vector<LedgerEntry> accounts(50);

        std::unordered_map<AccountID, LedgerEntry> accountsMap;

        for (auto& l : accounts)
        {
            l.data.type(ACCOUNT);
            auto& a = l.data.account();
            a = LedgerTestUtils::generateValidAccountEntry(5);
            accountsMap.insert(std::make_pair(a.accountID, l));
        }

        LedgerHeader lh;
        LedgerDelta delta(lh, db, false);

        // adding accounts
        for (auto const& l : accountsMap)
        {
            AccountFrame::pointer af = std::make_shared<AccountFrame>(l.second);
            af->storeAdd(delta, db);
            auto fromDb = AccountFrame::loadAccount(af->getID(), db);
            REQUIRE(af->getAccount() == fromDb->getAccount());
        }
        app->getLedgerManager().checkDbState();

        // updating accounts
        for (auto& l : accountsMap)
        {
            AccountEntry& newA = l.second.data.account();
            // replace by completely new object
            newA = LedgerTestUtils::generateValidAccountEntry(5);

            // preserve the accountID as it's the key
            newA.accountID = l.first;

            AccountFrame::pointer af = std::make_shared<AccountFrame>(l.second);
            af->storeChange(delta, db);
            auto fromDb = AccountFrame::loadAccount(af->getID(), db);
            REQUIRE(af->getAccount() == fromDb->getAccount());
        }
        app->getLedgerManager().checkDbState();

        // create a bunch of trust lines
        std::unordered_map<AccountID, std::vector<TrustFrame::pointer>>
            trustLinesMap;

        autocheck::generator<uint8_t> intGen;

        auto entriesProcessor =
            [&](std::function<void(LedgerEntry&)> accountProc) {
                for (auto& l : accountsMap)
                {
                    accountProc(l.second);

                    AccountFrame::pointer af =
                        std::make_shared<AccountFrame>(l.second);
                    af->storeChange(delta, db);
                    auto fromDb = AccountFrame::loadAccount(af->getID(), db);
                    REQUIRE(af->getAccount() == fromDb->getAccount());
                }
            };

        auto trustLineProcessor = [&](std::function<int(LedgerEntry&)> proc) {
            entriesProcessor([&](LedgerEntry& account) {
                AccountEntry& newA = account.data.account();
                uint8_t nbLines = intGen() % 32;
                for (uint8_t i = 0; i < nbLines; i++)
                {
                    LedgerEntry le;
                    le.data.type(TRUSTLINE);
                    auto& tl = le.data.trustLine();
                    tl = LedgerTestUtils::generateValidTrustLineEntry(5);
                    tl.accountID = newA.accountID;
                    newA.numSubEntries += proc(le);
                }
            });
        };

        // create a bunch of trust lines
        trustLineProcessor([&](LedgerEntry& le) {
            auto& lines = trustLinesMap[le.data.trustLine().accountID];

            LedgerKey thisKey = LedgerEntryKey(le);

            if (std::find_if(lines.begin(), lines.end(),
                             [&thisKey](TrustFrame::pointer tf) {
                                 return thisKey == tf->getKey();
                             }) == lines.end())
            {
                auto tfp = std::make_shared<TrustFrame>(le);
                tfp->storeAdd(delta, db);
                lines.emplace_back(tfp);
                return 1;
            }
            return 0;
        });

        app->getLedgerManager().checkDbState();

        // modify trust lines
        trustLineProcessor([&](LedgerEntry& le) {
            auto& lines = trustLinesMap[le.data.trustLine().accountID];
            if (lines.size() != 0)
            {
                size_t indexToChange = intGen() % lines.size();
                auto tfp = lines[indexToChange];
                // change all but the asset
                le.data.trustLine().asset = tfp->getTrustLine().asset;
                tfp->mEntry = le;
                tfp->storeChange(delta, db);
                auto& thisTL = tfp->getTrustLine();
                auto fromDb = TrustFrame::loadTrustLine(thisTL.accountID,
                                                        thisTL.asset, db);
                REQUIRE(thisTL == fromDb->getTrustLine());
            }
            return 0;
        });

        app->getLedgerManager().checkDbState();

        std::unordered_map<AccountID, std::vector<OfferFrame::pointer>>
            offerMap;
        std::unordered_set<uint64> offerIDs;

        auto offerProcessor = [&](std::function<int(LedgerEntry&)> proc) {
            entriesProcessor([&](LedgerEntry& account) {
                AccountEntry& newA = account.data.account();
                uint8_t nbOffers = intGen() % 32;
                for (uint8_t i = 0; i < nbOffers; i++)
                {
                    LedgerEntry le;
                    le.data.type(OFFER);
                    auto& of = le.data.offer();
                    of = LedgerTestUtils::generateValidOfferEntry(5000);
                    of.sellerID = newA.accountID;
                    newA.numSubEntries += proc(le);
                }
            });
        };

        // create a bunch of offers
        offerProcessor([&](LedgerEntry& le) {
            auto& offers = offerMap[le.data.offer().sellerID];

            LedgerKey thisKey = LedgerEntryKey(le);

            if (std::find(offerIDs.begin(), offerIDs.end(),
                          le.data.offer().offerID) == offerIDs.end())
            {
                auto off = std::make_shared<OfferFrame>(le);
                off->storeAdd(delta, db);
                offers.emplace_back(off);
                offerIDs.insert(off->getOfferID());
                return 1;
            }
            return 0;
        });

        app->getLedgerManager().checkDbState();

        // modify offers
        offerProcessor([&](LedgerEntry& le) {
            auto& offers = offerMap[le.data.offer().sellerID];
            if (offers.size() != 0)
            {
                size_t indexToChange = intGen() % offers.size();
                auto off = offers[indexToChange];
                // change all but sellerID and OfferID

                auto& newO = le.data.offer();

                auto& thisO = off->getOffer();

                newO.offerID = thisO.offerID;
                newO.sellerID = thisO.sellerID;

                thisO = newO;

                off->storeChange(delta, db);
                auto fromDb =
                    OfferFrame::loadOffer(thisO.sellerID, thisO.offerID, db);
                REQUIRE(thisO == fromDb->getOffer());
            }
            return 0;
        });

        app->getLedgerManager().checkDbState();

        // delete offers
        for (auto& ofl : offerMap)
        {
            LedgerEntry& ale = accountsMap[ofl.first];
            for (auto off : ofl.second)
            {
                off->storeDelete(delta, db);
                ale.data.account().numSubEntries--;
            }
            AccountFrame::pointer af = std::make_shared<AccountFrame>(ale);
            af->storeChange(delta, db);
            auto fromDb = AccountFrame::loadAccount(af->getID(), db);
            REQUIRE(af->getAccount() == fromDb->getAccount());
        }

        app->getLedgerManager().checkDbState();

        // delete trust lines
        for (auto& atl : trustLinesMap)
        {
            LedgerEntry& ale = accountsMap[atl.first];
            for (auto tf : atl.second)
            {
                tf->storeDelete(delta, db);
                ale.data.account().numSubEntries--;
            }
            AccountFrame::pointer af = std::make_shared<AccountFrame>(ale);
            af->storeChange(delta, db);
            auto fromDb = AccountFrame::loadAccount(af->getID(), db);
            REQUIRE(af->getAccount() == fromDb->getAccount());
        }

        app->getLedgerManager().checkDbState();

        // deleting accounts
        for (auto const& l : accountsMap)
        {
            AccountFrame::pointer af = std::make_shared<AccountFrame>(l.second);
            REQUIRE(AccountFrame::loadAccount(af->getID(), db) != nullptr);
            REQUIRE(AccountFrame::exists(db, af->getKey()));
            af->storeDelete(delta, db);
            REQUIRE(AccountFrame::loadAccount(af->getID(), db) == nullptr);
            REQUIRE(!AccountFrame::exists(db, af->getKey()));
        }

        app->getLedgerManager().checkDbState();
    }
}
}
