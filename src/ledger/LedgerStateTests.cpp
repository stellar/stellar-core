// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerEntryReference.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "ledger/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include <xdrpp/autocheck.h>

// TODO(jonjove): Remove this header (used for LedgerEntryKey)
#include "ledger/EntryFrame.h"

using namespace stellar;

LedgerEntry
generateLedgerEntry(LedgerEntryType let)
{
    LedgerEntry le;
    do
    {
        le = LedgerTestUtils::generateValidLedgerEntry();
    } while (le.data.type() != let);
    return le;
}

LedgerEntry
generateLedgerEntryWithSameKey(LedgerEntry const& entry)
{
    LedgerEntryType let = entry.data.type();
    auto le = generateLedgerEntry(let);
    switch (let)
    {
    case ACCOUNT:
        le.data.account().accountID = entry.data.account().accountID;
        break;
    case TRUSTLINE:
        le.data.trustLine().accountID = entry.data.trustLine().accountID;
        le.data.trustLine().asset = entry.data.trustLine().asset;
        break;
    case OFFER:
        le.data.offer().sellerID = entry.data.offer().sellerID;
        le.data.offer().offerID = entry.data.offer().offerID;
        break;
    case DATA:
        le.data.data().accountID = entry.data.data().accountID;
        le.data.data().dataName = entry.data.data().dataName;
        break;
    default:
        abort();
    }
    return le;
}

TEST_CASE("LedgerState::commit", "[ledger][ledgerstate][commit]")
{
    for (auto val : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        LedgerEntryType let = static_cast<LedgerEntryType>(val);
        std::string letStr = xdr::xdr_traits<LedgerEntryType>::enum_name(let);
        SECTION(letStr)
        {
            SECTION("[LedgerState::commit] if (mParent) { ... }")
            {
                VirtualClock clock;
                Config cfg = getTestConfig();
                cfg.INVARIANT_CHECKS = {};
                Application::pointer app = createTestApplication(clock, cfg);

                auto leParent1 = generateLedgerEntry(let);
                auto leParent2 = generateLedgerEntryWithSameKey(leParent1);
                auto parentKey = LedgerEntryKey(leParent1);
                REQUIRE(parentKey == LedgerEntryKey(leParent2));

                auto leChild = generateLedgerEntry(let);
                auto childKey = LedgerEntryKey(leChild);

                LedgerStateRoot& root(app->getLedgerStateRoot());
                {
                    LedgerState lsParent(root);
                    lsParent.create(leParent1);
                    {
                        LedgerState lsChild(lsParent);
                        auto lerParent = lsChild.load(parentKey);
                        auto lerChild = lsChild.create(leChild);
                        *lerParent->entry() = leParent2;
                        lsChild.commit();

                        REQUIRE(!lerParent->valid());
                        REQUIRE(!lerChild->valid());
                    }

                    auto lerParent = lsParent.load(parentKey);
                    REQUIRE(*lerParent->entry() == leParent2);
                    REQUIRE(lerParent->previousEntry() == nullptr);

                    auto lerChild = lsParent.load(childKey);
                    REQUIRE(*lerChild->entry() == leChild);
                    REQUIRE(lerChild->previousEntry() == nullptr);
                }
            }
            SECTION("[LedgerState::commit] else { ... }")
            {
                VirtualClock clock;
                Config cfg = getTestConfig();
                cfg.INVARIANT_CHECKS = {};
                Application::pointer app = createTestApplication(clock, cfg);

                auto le = generateLedgerEntry(let);
                auto key = LedgerEntryKey(le);

                LedgerStateRoot& root(app->getLedgerStateRoot());
                {
                    LedgerState ls(root);
                    ls.create(le);
                    ls.commit();
                }
                {
                    LedgerState ls(root);
                    auto ler = ls.load(key);
                    REQUIRE(*ler->entry() == le);
                    REQUIRE(ler->previousEntry() != nullptr);
                    REQUIRE(*ler->previousEntry() == le);
                    ler->erase();
                    ls.commit();
                }
                {
                    LedgerState ls(root);
                    REQUIRE_THROWS_WITH(ls.load(key),
                                        "Key does not exist in database");
                }
            }
        }
    }
}

TEST_CASE("LedgerState::create", "[ledger][ledgerstate][create]")
{
    for (auto val : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        LedgerEntryType let = static_cast<LedgerEntryType>(val);
        std::string letStr = xdr::xdr_traits<LedgerEntryType>::enum_name(let);
        SECTION(letStr)
        {
            SECTION(
                "[LedgerState::createHelper] if (iter != mState.end()) { ... }")
            {
                VirtualClock clock;
                Config cfg = getTestConfig();
                cfg.INVARIANT_CHECKS = {};
                Application::pointer app = createTestApplication(clock, cfg);

                auto le = generateLedgerEntry(let);
                auto key = LedgerEntryKey(le);

                LedgerStateRoot& root(app->getLedgerStateRoot());
                {
                    LedgerState ls(root);
                    auto ler = ls.create(le);
                    REQUIRE_THROWS_WITH(ls.create(le),
                                        "Key already exists in memory");

                    ler->erase();
                    ler = ls.create(le);
                    REQUIRE(*ler->entry() == le);
                    REQUIRE(ler->previousEntry() == nullptr);
                }
            }
            SECTION("[LedgerState::createHelper] else { ... }")
            {
                VirtualClock clock;
                Config cfg = getTestConfig();
                cfg.INVARIANT_CHECKS = {};
                Application::pointer app = createTestApplication(clock, cfg);

                auto le = generateLedgerEntry(let);
                auto key = LedgerEntryKey(le);

                LedgerStateRoot& root(app->getLedgerStateRoot());
                {
                    LedgerState ls(root);
                    auto ler = ls.create(le);
                    REQUIRE(*ler->entry() == le);
                    REQUIRE(ler->previousEntry() == nullptr);
                    ls.commit();
                }
                {
                    LedgerState ls(root);
                    REQUIRE_THROWS_WITH(ls.create(le),
                                        "Key already exists in database");
                }
            }
        }
    }
}

TEST_CASE("LedgerState::load", "[ledger][ledgerstate][load]")
{
    for (auto val : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        LedgerEntryType let = static_cast<LedgerEntryType>(val);
        std::string letStr = xdr::xdr_traits<LedgerEntryType>::enum_name(let);
        SECTION(letStr)
        {
            SECTION(
                "[LedgerState::loadHelper] if (iter != mState.end()) { ... }")
            {
                VirtualClock clock;
                Config cfg = getTestConfig();
                cfg.INVARIANT_CHECKS = {};
                Application::pointer app = createTestApplication(clock, cfg);

                auto le = generateLedgerEntry(let);
                auto le2 = generateLedgerEntryWithSameKey(le);
                auto key = LedgerEntryKey(le);
                REQUIRE(key == LedgerEntryKey(le2));

                LedgerStateRoot& root(app->getLedgerStateRoot());
                {
                    LedgerState ls(root);
                    auto ler = ls.create(le);
                    REQUIRE_THROWS_WITH(ls.load(key),
                                        "Valid LedgerEntryReference already "
                                        "exists for this key");
                    ler->erase();
                    ler->invalidate();
                    REQUIRE_THROWS_WITH(
                        ls.load(key),
                        "Key exists in memory but LedgerEntry has been erased");

                    ler = ls.create(le);
                    *ler->entry() = le2;
                    ler->invalidate();
                    ler = ls.load(key);
                    REQUIRE(*ler->entry() == le2);
                    REQUIRE(ler->previousEntry() == nullptr);

                    {
                        LedgerState lsChild(ls);
                        auto lerChild = lsChild.load(key);
                        REQUIRE(*lerChild->entry() == le2);
                        REQUIRE(lerChild->previousEntry() != nullptr);
                        REQUIRE(*lerChild->previousEntry() == le2);
                    }
                }
            }
        }
    }
}

// TODO(jonjove): LedgerState::LoadBestOfferContext invalidation

TEST_CASE("LedgerState::loadBestOffer matches database order",
          "[ledger][ledgerstate][loadbestoffer]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {};
    Application::pointer app = createTestApplication(clock, cfg);

    auto generateOffer = []() {
        auto le = generateLedgerEntry(OFFER);
        le.data.offer().selling.alphaNum4().issuer = PublicKey();
        le.data.offer().buying.alphaNum4().issuer = PublicKey();
        return le;
    };
    auto extractBestOffers = [](LedgerState& ls, Asset const& selling,
                                Asset const& buying) {
        std::vector<LedgerEntry> bestOffers;
        std::shared_ptr<LedgerEntryReference> ler;
        while ((ler = ls.loadBestOffer(selling, buying)) != nullptr)
        {
            bestOffers.push_back(*ler->entry());
            ler->erase();
            ler->invalidate();
        }
        return bestOffers;
    };

    size_t nOffers = 13;
    std::vector<LedgerEntry> offers1;
    std::vector<LedgerEntry> offers2;
    std::generate_n(std::back_inserter(offers1), nOffers, generateOffer);
    std::generate_n(std::back_inserter(offers2), nOffers, generateOffer);

    Asset const& selling = offers1.front().data.offer().selling;
    Asset const& buying = offers1.front().data.offer().buying;

    std::vector<LedgerEntry> memoryBestOffers;
    std::vector<LedgerEntry> mixedBestOffers;
    std::vector<LedgerEntry> dbBestOffers;

    LedgerStateRoot& root(app->getLedgerStateRoot());
    {
        LedgerState ls(root);
        for (auto const& le : offers1)
        {
            ls.create(le)->invalidate();
        }
        for (auto const& le : offers2)
        {
            ls.create(le)->invalidate();
        }
        memoryBestOffers = extractBestOffers(ls, selling, buying);
        REQUIRE(memoryBestOffers.size() == 2 * nOffers);
    }
    {
        LedgerState ls1(root);
        for (auto const& le : offers1)
        {
            ls1.create(le)->invalidate();
        }
        ls1.commit();

        LedgerState ls2(root);
        for (auto const& le : offers2)
        {
            ls2.create(le)->invalidate();
        }
        mixedBestOffers = extractBestOffers(ls2, selling, buying);
        REQUIRE(mixedBestOffers.size() == 2 * nOffers);
    }
    {
        LedgerState ls1(root);
        for (auto const& le : offers2)
        {
            ls1.create(le)->invalidate();
        }
        ls1.commit();

        LedgerState ls2(root);
        dbBestOffers = extractBestOffers(ls2, selling, buying);
        REQUIRE(dbBestOffers.size() == 2 * nOffers);
    }

    REQUIRE(memoryBestOffers == dbBestOffers);
    REQUIRE(mixedBestOffers == dbBestOffers);
}

TEST_CASE("LedgerState::loadBestOffer in database but modified in memory",
          "[ledger][ledgerstate][loadbestoffer]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {};
    Application::pointer app = createTestApplication(clock, cfg);

    auto generateOffer = []() {
        auto le = generateLedgerEntry(OFFER);
        le.data.offer().selling.alphaNum4().issuer = PublicKey();
        le.data.offer().buying.alphaNum4().issuer = PublicKey();
        return le;
    };
    auto generateOfferWithSameKey = [](LedgerEntry const& entry) {
        auto le = generateLedgerEntryWithSameKey(entry);
        le.data.offer().selling.alphaNum4().issuer = PublicKey();
        le.data.offer().buying.alphaNum4().issuer = PublicKey();
        return le;
    };

    auto le1 = generateOffer();
    auto le2 = generateOfferWithSameKey(le1);

    Asset const& selling = le1.data.offer().selling;
    Asset const& buying = le1.data.offer().buying;

    LedgerStateRoot& root(app->getLedgerStateRoot());
    for (auto const& entries :
         {std::make_pair(le1, le2), std::make_pair(le2, le1)})
    {
        {
            LedgerState ls(root);
            ls.create(entries.first);
            ls.commit();
        }
        {
            LedgerState ls(root);
            auto ler = ls.load(LedgerEntryKey(entries.first));
            *ler->entry() = entries.second;
            ler->invalidate();

            auto lerBestOffer = ls.loadBestOffer(selling, buying);
            REQUIRE(*lerBestOffer->entry() == entries.second);
            lerBestOffer->erase();
            ls.commit();
        }
    }
}

TEST_CASE("LedgerState::loadInflationWinners",
          "[ledger][ledgerstate][inflation]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {};
    Application::pointer app = createTestApplication(clock, cfg);

    size_t nAccounts = 13;
    size_t nAccountsChanged = 6;
    std::vector<LedgerEntry> accounts;
    std::vector<LedgerEntry> accountsChanged;
    std::vector<LedgerEntry> accountsNew;
    std::generate_n(std::back_inserter(accounts), nAccounts,
                    std::bind(generateLedgerEntry, ACCOUNT));
    std::generate_n(std::back_inserter(accountsNew), nAccounts,
                    std::bind(generateLedgerEntry, ACCOUNT));
    std::transform(accounts.begin(), accounts.end(),
                   std::back_inserter(accountsChanged),
                   generateLedgerEntryWithSameKey);

    std::vector<InflationVotes> memoryInflationWinners;
    std::vector<InflationVotes> mixedInflationWinners;
    std::vector<InflationVotes> dbInflationWinners;

    LedgerStateRoot& root(app->getLedgerStateRoot());
    {
        LedgerState ls(root);
        for (auto const& le : accounts)
        {
            ls.create(le)->invalidate();
        }
        for (auto const& le : accountsNew)
        {
            ls.create(le)->invalidate();
        }
        for (auto const& le : accountsChanged)
        {
            auto ler = ls.load(LedgerEntryKey(le));
            *ler->entry() = le;
            ler->invalidate();
        }
        memoryInflationWinners = ls.loadInflationWinners(10, 0);
        REQUIRE(memoryInflationWinners.size() == 10);
    }
    {
        LedgerState ls1(root);
        for (auto const& le : accounts)
        {
            ls1.create(le)->invalidate();
        }
        ls1.commit();

        LedgerState ls2(root);
        for (auto const& le : accountsNew)
        {
            ls2.create(le)->invalidate();
        }
        for (auto const& le : accountsChanged)
        {
            auto ler = ls2.load(LedgerEntryKey(le));
            *ler->entry() = le;
            ler->invalidate();
        }
        mixedInflationWinners = ls2.loadInflationWinners(10, 0);
        REQUIRE(mixedInflationWinners.size() == 10);
        ls2.commit();
    }
    {
        LedgerState ls(root);
        dbInflationWinners = ls.loadInflationWinners(10, 0);
        REQUIRE(dbInflationWinners.size() == 10);
    }
    REQUIRE(memoryInflationWinners == dbInflationWinners);
    REQUIRE(mixedInflationWinners == dbInflationWinners);
}

// TODO(jonjove): Might be a few more cases worth testing here
TEST_CASE("LedgerState::rollback", "[ledger][ledgerstate][rollback]")
{
    for (auto val : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        LedgerEntryType let = static_cast<LedgerEntryType>(val);
        std::string letStr = xdr::xdr_traits<LedgerEntryType>::enum_name(let);
        SECTION(letStr)
        {
            VirtualClock clock;
            Config cfg = getTestConfig();
            cfg.INVARIANT_CHECKS = {};
            Application::pointer app = createTestApplication(clock, cfg);

            auto le1 = generateLedgerEntry(let);
            auto le2 = generateLedgerEntryWithSameKey(le1);
            auto key = LedgerEntryKey(le1);
            REQUIRE(key == LedgerEntryKey(le2));

            LedgerStateRoot& root(app->getLedgerStateRoot());
            {
                LedgerState ls(root);
                ls.create(le1);
                ls.rollback();
            }
            {
                LedgerState ls(root);
                REQUIRE_THROWS_WITH(ls.load(key),
                                    "Key does not exist in database");
            }

            {
                LedgerState ls(root);
                ls.create(le1);
                {
                    LedgerState lsChild(ls);
                    *lsChild.load(key)->entry() = le2;
                    lsChild.rollback();
                }
                REQUIRE(*ls.load(key)->entry() == le1);
            }
        }
    }
}

TEST_CASE("Exception during LedgerState::commit leaves cache valid",
          "[ledger][ledgerstate][flushcache]")
{
    for (auto val : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        LedgerEntryType let = static_cast<LedgerEntryType>(val);
        std::string letStr = xdr::xdr_traits<LedgerEntryType>::enum_name(let);
        SECTION(letStr)
        {
            VirtualClock clock;
            Config cfg = getTestConfig();
            cfg.INVARIANT_CHECKS = {};
            Application::pointer app = createTestApplication(clock, cfg);

            auto le = generateLedgerEntry(let);
            auto key = LedgerEntryKey(le);

            LedgerStateRoot& root(app->getLedgerStateRoot());
            {
                LedgerState ls(root);
                ls.create(le);
                ls.commit();
            }
            {
                LedgerState ls(root);
                ls.load(key)->erase();
                REQUIRE_THROWS_AS(ls.commit([]() {
                    throw std::runtime_error("Abort database commit");
                }),
                                  std::runtime_error);
            }
            {
                LedgerState ls(root);
                REQUIRE_THROWS_AS(ls.create(le), std::runtime_error);
            }
        }
    }
}

TEST_CASE("LedgerState cannot load or create TrustLine if accountID is issuer",
          "[ledger][ledgerstate][issuer]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {};
    Application::pointer app = createTestApplication(clock, cfg);

    LedgerEntry le;
    do
    {
        le = generateLedgerEntry(TRUSTLINE);
    } while (le.data.trustLine().asset.type() == ASSET_TYPE_NATIVE);
    le.data.trustLine().accountID = getIssuer(le.data.trustLine().asset);

    LedgerState ls(app->getLedgerStateRoot());
    REQUIRE_THROWS_WITH(ls.create(le), "TrustLine accountID is issuer");
    REQUIRE_THROWS_WITH(ls.load(LedgerEntryKey(le)),
                        "TrustLine accountID is issuer");
}

TEST_CASE("LedgerState::loadHeader", "[ledger][ledgerstate][loadheader]")
{
    auto generateLedgerHeader = autocheck::map(
        [](LedgerHeader&& lh, size_t s) {
            lh.ledgerSeq &= INT32_MAX;
            lh.scpValue.closeTime &= INT64_MAX;
            return lh;
        },
        autocheck::generator<LedgerHeader>());

    VirtualClock clock;
    Config cfg = getTestConfig();
    cfg.INVARIANT_CHECKS = {};
    Application::pointer app = createTestApplication(clock, cfg);

    {
        LedgerState ls(app->getLedgerStateRoot());
        ls.loadHeader()->header() = generateLedgerHeader();
        ls.commit();
    }

    {
        LedgerState ls(app->getLedgerStateRoot());
        auto lhr1 = ls.loadHeader();
        auto lh1 = lhr1->header();
        lhr1->invalidate();

        auto lhr2 = ls.loadHeader();
        auto lh2 = lhr2->header();
        lhr2->invalidate();

        REQUIRE(lh1 == lh2);
    }

    {
        LedgerState ls(app->getLedgerStateRoot());
        auto lh1 = ls.loadHeader()->header();
        LedgerHeader lh2;
        {
            LedgerState lsChild(ls);
            auto lhr = lsChild.loadHeader();
            REQUIRE(lh1 == lhr->header());
            lhr->header() = generateLedgerHeader();
            lh2 = lhr->header();
            lsChild.commit();
        }
        REQUIRE(ls.loadHeader()->header() == lh2);
    }
}

// TODO(jonjove): Might be a few other cases worth testing
TEST_CASE("LedgerState::forget and LedgerEntryReference::forgetFromLedgerState",
          "[ledger][ledgerstate][forget]")
{
    for (auto val : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        LedgerEntryType let = static_cast<LedgerEntryType>(val);
        std::string letStr = xdr::xdr_traits<LedgerEntryType>::enum_name(let);
        SECTION(letStr)
        {
            VirtualClock clock;
            Config cfg = getTestConfig();
            cfg.INVARIANT_CHECKS = {};
            Application::pointer app = createTestApplication(clock, cfg);

            auto le = generateLedgerEntry(let);
            auto key = LedgerEntryKey(le);

            LedgerState ls(app->getLedgerStateRoot());
            ls.create(le)->forgetFromLedgerState();
            REQUIRE_THROWS_AS(ls.load(key), std::runtime_error);
            {
                LedgerState lsChild(ls);
                lsChild.create(le)->forgetFromLedgerState();
                lsChild.commit();
            }
            REQUIRE_THROWS_AS(ls.load(key), std::runtime_error);
        }
    }
}
