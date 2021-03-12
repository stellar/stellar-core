#include "ledger/InternalLedgerEntry.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnImpl.h"
#include "util/UnorderedMap.h"
#include "xdr/Stellar-ledger-entries.h"
#include <map>
#include <set>
#include <vector>

// This is a stub helper class that pretends to implements a "root"
// AbstractLedgerTxnParent like LedgerTxnRoot but returns empty/null values for
// any query made of it, and throws if anyone ever tries to commit to it.
//
// This is used to anchor a live-but-never-committed LedgerTxn when doing
// strictly-in-memory fast history replay.

namespace stellar
{

class InMemoryLedgerTxnRoot : public AbstractLedgerTxnParent
{
    std::unique_ptr<LedgerHeader> mHeader;

#ifdef BEST_OFFER_DEBUGGING
    bool const mBestOfferDebuggingEnabled;
#endif

  public:
    InMemoryLedgerTxnRoot(
#ifdef BEST_OFFER_DEBUGGING
        bool bestOfferDebuggingEnabled
#endif
    );
    void addChild(AbstractLedgerTxn& child) override;
    void commitChild(EntryIterator iter, LedgerTxnConsistency cons) override;
    void rollbackChild() override;

    UnorderedMap<LedgerKey, LedgerEntry> getAllOffers() override;
    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling) override;
    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 OfferDescriptor const& worseThan) override;
    UnorderedMap<LedgerKey, LedgerEntry>
    getOffersByAccountAndAsset(AccountID const& account,
                               Asset const& asset) override;

    LedgerHeader const& getHeader() const override;

    std::vector<InflationWinner>
    getInflationWinners(size_t maxWinners, int64_t minBalance) override;

    std::shared_ptr<InternalLedgerEntry const>
    getNewestVersion(InternalLedgerKey const& key) const override;

    uint64_t countObjects(LedgerEntryType let) const override;
    uint64_t countObjects(LedgerEntryType let,
                          LedgerRange const& ledgers) const override;

    void deleteObjectsModifiedOnOrAfterLedger(uint32_t ledger) const override;

    void dropAccounts() override;
    void dropData() override;
    void dropOffers() override;
    void dropTrustLines() override;
    void dropClaimableBalances() override;
    double getPrefetchHitRate() const override;
    uint32_t prefetch(UnorderedSet<LedgerKey> const& keys) override;
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    void resetForFuzzer() override;
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

#ifdef BEST_OFFER_DEBUGGING
    bool bestOfferDebuggingEnabled() const override;

    std::shared_ptr<LedgerEntry const>
    getBestOfferSlow(Asset const& buying, Asset const& selling,
                     OfferDescriptor const* worseThan,
                     std::unordered_set<int64_t>& exclude) override;
#endif
};
}
