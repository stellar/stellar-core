#include "ledger/InMemoryLedgerTxnRoot.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerTxn.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdrpp/marshal.h"
#include <algorithm>

namespace stellar
{

InMemoryLedgerTxnRoot::InMemoryLedgerTxnRoot()
    : mHeader(std::make_unique<LedgerHeader>())
{
}

void
InMemoryLedgerTxnRoot::addChild(AbstractLedgerTxn& child)
{
}

void
InMemoryLedgerTxnRoot::commitChild(EntryIterator iter,
                                   LedgerTxnConsistency cons)
{
    throw std::runtime_error("committing to stub InMemoryLedgerTxnRoot");
}

void
InMemoryLedgerTxnRoot::rollbackChild()
{
}

std::unordered_map<LedgerKey, LedgerEntry>
InMemoryLedgerTxnRoot::getAllOffers()
{
    return std::unordered_map<LedgerKey, LedgerEntry>();
}

std::shared_ptr<LedgerEntry const>
InMemoryLedgerTxnRoot::getBestOffer(Asset const& buying, Asset const& selling)
{
    return nullptr;
}

std::shared_ptr<LedgerEntry const>
InMemoryLedgerTxnRoot::getBestOffer(Asset const& buying, Asset const& selling,
                                    OfferDescriptor const& worseThan)
{
    return nullptr;
}

std::unordered_map<LedgerKey, LedgerEntry>
InMemoryLedgerTxnRoot::getOffersByAccountAndAsset(AccountID const& account,
                                                  Asset const& asset)
{
    return std::unordered_map<LedgerKey, LedgerEntry>();
}

LedgerHeader const&
InMemoryLedgerTxnRoot::getHeader() const
{
    return *mHeader;
}

std::vector<InflationWinner>
InMemoryLedgerTxnRoot::getInflationWinners(size_t maxWinners,
                                           int64_t minBalance)
{
    return std::vector<InflationWinner>();
}

std::shared_ptr<GeneralizedLedgerEntry const>
InMemoryLedgerTxnRoot::getNewestVersion(GeneralizedLedgerKey const& key) const
{
    return nullptr;
}

uint64_t
InMemoryLedgerTxnRoot::countObjects(LedgerEntryType let) const
{
    return 0;
}

uint64_t
InMemoryLedgerTxnRoot::countObjects(LedgerEntryType let,
                                    LedgerRange const& ledgers) const
{
    return 0;
}

void
InMemoryLedgerTxnRoot::deleteObjectsModifiedOnOrAfterLedger(
    uint32_t ledger) const
{
}

void
InMemoryLedgerTxnRoot::dropAccounts()
{
}

void
InMemoryLedgerTxnRoot::dropData()
{
}

void
InMemoryLedgerTxnRoot::dropOffers()
{
}

void
InMemoryLedgerTxnRoot::dropTrustLines()
{
}

void
InMemoryLedgerTxnRoot::dropClaimableBalances()
{
}

double
InMemoryLedgerTxnRoot::getPrefetchHitRate() const
{
    return 0.0;
}

uint32_t
InMemoryLedgerTxnRoot::prefetch(std::unordered_set<LedgerKey> const& keys)
{
    return 0;
}
}
