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

InMemoryLedgerTxnRoot::InMemoryLedgerTxnRoot(
#ifdef BEST_OFFER_DEBUGGING
    bool bestOfferDebuggingEnabled
#endif
    )
    : mHeader(std::make_unique<LedgerHeader>())
#ifdef BEST_OFFER_DEBUGGING
    , mBestOfferDebuggingEnabled(bestOfferDebuggingEnabled)
#endif
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

UnorderedMap<LedgerKey, LedgerEntry>
InMemoryLedgerTxnRoot::getAllOffers()
{
    return UnorderedMap<LedgerKey, LedgerEntry>();
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

UnorderedMap<LedgerKey, LedgerEntry>
InMemoryLedgerTxnRoot::getOffersByAccountAndAsset(AccountID const& account,
                                                  Asset const& asset)
{
    return UnorderedMap<LedgerKey, LedgerEntry>();
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

std::shared_ptr<InternalLedgerEntry const>
InMemoryLedgerTxnRoot::getNewestVersion(InternalLedgerKey const& key) const
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
InMemoryLedgerTxnRoot::prefetch(UnorderedSet<LedgerKey> const& keys)
{
    return 0;
}

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
void
InMemoryLedgerTxnRoot::resetForFuzzer()
{
    abort();
}
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

#ifdef BEST_OFFER_DEBUGGING
bool
InMemoryLedgerTxnRoot::bestOfferDebuggingEnabled() const
{
    return mBestOfferDebuggingEnabled;
}

std::shared_ptr<LedgerEntry const>
InMemoryLedgerTxnRoot::getBestOfferSlow(Asset const& buying,
                                        Asset const& selling,
                                        OfferDescriptor const* worseThan,
                                        std::unordered_set<int64_t>& exclude)
{
    return nullptr;
}
#endif
}
