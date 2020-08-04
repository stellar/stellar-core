#include "ledger/GeneralizedLedgerEntry.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnImpl.h"
#include "xdr/Stellar-ledger-entries.h"
#include <map>
#include <set>
#include <unordered_map>
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

  public:
    InMemoryLedgerTxnRoot();
    void addChild(AbstractLedgerTxn& child) override;
    void commitChild(EntryIterator iter, LedgerTxnConsistency cons) override;
    void rollbackChild() override;

    std::unordered_map<LedgerKey, LedgerEntry> getAllOffers() override;
    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling) override;
    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 OfferDescriptor const& worseThan) override;
    std::unordered_map<LedgerKey, LedgerEntry>
    getOffersByAccountAndAsset(AccountID const& account,
                               Asset const& asset) override;

    LedgerHeader const& getHeader() const override;

    std::vector<InflationWinner>
    getInflationWinners(size_t maxWinners, int64_t minBalance) override;

    std::shared_ptr<GeneralizedLedgerEntry const>
    getNewestVersion(GeneralizedLedgerKey const& key) const override;

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
    uint32_t prefetch(std::unordered_set<LedgerKey> const& keys) override;
};
}
