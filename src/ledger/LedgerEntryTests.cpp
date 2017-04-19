// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledgerdelta/LedgerDeltaLayer.h"
#include "crypto/SecretKey.h"
#include "database/AccountQueries.h"
#include "database/Database.h"
#include "database/OfferQueries.h"
#include "database/TrustLineQueries.h"
#include "ledger/AccountFrame.h"
#include "ledger/LedgerEntries.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTestUtils.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "ledgerdelta/LedgerDelta.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "xdrpp/autocheck.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"

#include <memory>
#include <unordered_map>
#include <utility>

using namespace stellar;

namespace Catch
{

template <>
std::string
toString(LedgerEntry const& le)
{
    return xdr::xdr_to_string(le);
}
}

namespace LedgerEntryTests
{

TEST_CASE("Ledger Entry tests", "[ledgerentry]")
{
    Config cfg(getTestConfig(0));

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);
    app->start();
    auto& entries = app->getLedgerEntries();
    Database& db = app->getDatabase();

    SECTION("round trip with database")
    {
        std::vector<LedgerEntry> accounts(100);

        std::unordered_map<AccountID, LedgerEntry> accountsMap;

        for (auto& l : accounts)
        {
            l.data.type(ACCOUNT);
            auto& a = l.data.account();
            a = LedgerTestUtils::generateValidAccountEntry(5);
            l.lastModifiedLedgerSeq = 1;
            accountsMap.insert(std::make_pair(a.accountID, l));
        }

        LedgerHeader lh;
        lh.ledgerSeq = 1;

        // adding accounts
        for (auto const& l : accountsMap)
        {
            LedgerDelta delta(lh, entries);
            auto f = EntryFrame{l.second};
            delta.addEntry(f);
            app->getLedgerManager().apply(delta);
            auto fromDb = entries.load(accountKey(l.first));
            REQUIRE(l.second == *fromDb);
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

            LedgerDelta delta(lh, entries);
            auto f = EntryFrame{l.second};
            delta.updateEntry(f);
            app->getLedgerManager().apply(delta);
            auto fromDb = selectAccount(newA.accountID, app->getDatabase());
            REQUIRE(l.second == *fromDb);
        }
        app->getLedgerManager().checkDbState();

        // create a bunch of trust lines
        std::unordered_map<AccountID, std::vector<LedgerEntry>>
            trustLinesMap;

        autocheck::generator<uint8_t> intGen;

        auto entriesProcessor =
            [&](std::function<void(LedgerEntry&)> accountProc) {
                for (auto& l : accountsMap)
                {
                    accountProc(l.second);
                    auto account = AccountFrame{l.second};
                    LedgerDelta delta{lh, entries};
                    delta.updateEntry(account);
                    app->getLedgerManager().apply(delta);
                    auto fromDb = selectAccount(account.getAccountID(), app->getDatabase());
                    REQUIRE(l.second == *fromDb);
                }
            };

        auto trustLineProcessor = [&](std::function<int(LedgerEntry&)> proc) {
            entriesProcessor([&](LedgerEntry& account) {
                AccountEntry& newA = account.data.account();
                uint8_t nbLines = intGen() % 64;
                for (uint8_t i = 0; i < nbLines; i++)
                {
                    auto trust = TrustFrame{LedgerTestUtils::generateValidTrustLineEntry(5)};
                    trust.setAccountID(newA.accountID);
                    newA.numSubEntries += proc(trust.getEntry());
                }
            });
        };

        // create a bunch of trust lines
        trustLineProcessor([&](LedgerEntry& le) {
            auto trust = TrustFrame{le};
            auto& lines = trustLinesMap[trust.getAccountID()];

            LedgerKey thisKey = entryKey(le);

            if (std::find_if(lines.begin(), lines.end(),
                             [&thisKey](LedgerEntry const& tf) {
                                 return thisKey == entryKey(tf);
                             }) == lines.end())
            {
                auto f = EntryFrame{le};
                LedgerDelta delta(lh, entries);
                delta.addEntry(f);
                app->getLedgerManager().apply(delta);
                lines.emplace_back(le);
                return 1;
            }
            return 0;
        });

        app->getLedgerManager().checkDbState();

        // modify trust lines
        trustLineProcessor([&](LedgerEntry& le) {
            auto trust = TrustFrame{le};
            auto& lines = trustLinesMap[trust.getAccountID()];
            if (lines.size() != 0)
            {
                size_t indexToChange = intGen() % lines.size();
                auto tf = TrustFrame{lines[indexToChange]};
                // change all but the asset
                trust.setAsset(tf.getAsset());
                tf = trust;
                tf.getEntry().lastModifiedLedgerSeq = 1;
                le = trust.getEntry();
                LedgerDelta delta(lh, entries);
                delta.updateEntry(trust);
                app->getLedgerManager().apply(delta);
                auto fromDb = selectTrustLine(tf.getAccountID(), tf.getAsset(), app->getDatabase());
                REQUIRE(fromDb);
                REQUIRE(tf.getEntry() == *fromDb);
            }
            return 0;
        });

        app->getLedgerManager().checkDbState();

        std::unordered_map<AccountID, std::vector<LedgerEntry>> offerMap;
        std::unordered_set<uint64> offerIDs;

        auto offerProcessor = [&](std::function<int(LedgerEntry&)> proc) {
            entriesProcessor([&](LedgerEntry& account) {
                AccountEntry& newA = account.data.account();
                uint8_t nbOffers = intGen() % 64;
                for (uint8_t i = 0; i < nbOffers; i++)
                {
                    auto offer = OfferFrame{LedgerTestUtils::generateValidOfferEntry(5000)};
                    offer.setSellerID(newA.accountID);
                    newA.numSubEntries += proc(offer.getEntry());
                }
            });
        };

        // create a bunch of offers
        offerProcessor([&](LedgerEntry& le) {
            auto offer = OfferFrame{le};
            auto& offers = offerMap[offer.getSellerID()];

            LedgerKey thisKey = entryKey(le);

            if (std::find(offerIDs.begin(), offerIDs.end(),
                          offer.getOfferID()) == offerIDs.end())
            {
                auto f = EntryFrame{le};
                LedgerDelta delta(lh, entries);
                delta.addEntry(f);
                app->getLedgerManager().apply(delta);
                offers.emplace_back(le);
                offerIDs.insert(offer.getOfferID());
                return 1;
            }
            return 0;
        });

        app->getLedgerManager().checkDbState();

        // modify offers
        offerProcessor([&](LedgerEntry& le) {
            auto offer = OfferFrame{le};
            auto& offers = offerMap[offer.getSellerID()];
            if (offers.size() != 0)
            {
                size_t indexToChange = intGen() % offers.size();
                auto off = OfferFrame{offers[indexToChange]};
                // change all but sellerID and OfferID

                offer.setOfferID(off.getOfferID());
                offer.setSellerID(off.getSellerID());

                off = offer;
                offers[indexToChange] = off.getEntry();
                le = offer.getEntry();

                LedgerDelta delta(lh, entries);
                delta.updateEntry(off);
                app->getLedgerManager().apply(delta);
                auto fromDb = selectOffer(off.getSellerID(), off.getOfferID(), db);
                REQUIRE(off.getEntry() == *fromDb);
            }
            return 0;
        });

        app->getLedgerManager().checkDbState();

        // delete offers
        for (auto& ofl : offerMap)
        {
            auto account = AccountFrame{accountsMap[ofl.first]};
                LedgerDelta delta(lh, entries);
            for (auto off : ofl.second)
            {
                delta.deleteEntry(entryKey(off));
                account.addNumEntries(-1, app->getLedgerManager());
            }
            delta.updateEntry(account);
            app->getLedgerManager().apply(delta);
            accountsMap[ofl.first] = account.getEntry();
            auto fromDb = selectAccount(account.getAccountID(), app->getDatabase());
            REQUIRE(account == AccountFrame{*fromDb});
        }

        app->getLedgerManager().checkDbState();

        // delete trust lines
        for (auto& atl : trustLinesMap)
        {
            auto account = AccountFrame{accountsMap[atl.first]};
            LedgerDelta delta(lh, entries);
            for (auto tf : atl.second)
            {
                delta.deleteEntry(entryKey(tf));
                account.addNumEntries(-1, app->getLedgerManager());
            }
            delta.updateEntry(account);
            app->getLedgerManager().apply(delta);
            accountsMap[atl.first] = account.getEntry();
            auto fromDb = selectAccount(account.getAccountID(), app->getDatabase());
            REQUIRE(account == AccountFrame{*fromDb});
        }

        app->getLedgerManager().checkDbState();

        // deleting accounts
        for (auto const& l : accountsMap)
        {
            auto account = AccountFrame{l.second};
            auto id = account.getAccountID();
            auto key = account.getKey();
            LedgerDelta ledgerDelta(lh, entries);
            REQUIRE(ledgerDelta.loadAccount(id) != nullptr);
            REQUIRE(ledgerDelta.entryExists(key));
            ledgerDelta.deleteEntry(key);
            app->getLedgerManager().apply(ledgerDelta);

            LedgerDelta ledgerDelta2(lh, entries);
            REQUIRE(ledgerDelta2.loadAccount(id) == nullptr);
            REQUIRE(!ledgerDelta2.entryExists(key));
        }

        app->getLedgerManager().checkDbState();
    }
}
}
