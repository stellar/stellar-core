#pragma once

#include <map>
#include <set>
#include "ledger/EntryFrame.h"
#include "clf/LedgerCmp.h"
#include "xdrpp/marshal.h"

namespace stellar
{
class Application;

    class LedgerDelta
    {
        typedef std::map<LedgerKey, EntryFrame::pointer, LedgerEntryIdCmp> KeyEntryMap;

        KeyEntryMap mNew;
        KeyEntryMap mMod;
        std::set<LedgerKey, LedgerEntryIdCmp> mDelete;

        void addEntry(EntryFrame::pointer entry);
        void deleteEntry(EntryFrame::pointer entry);
        void modEntry(EntryFrame::pointer entry);

    public:

        void addEntry(EntryFrame const& entry);
        void deleteEntry(EntryFrame const& entry);
        void deleteEntry(LedgerKey const& key);
        void modEntry(EntryFrame const& entry);

        // apply other on top of delta, collapsing entries as appropriate
        void merge(LedgerDelta &other);

        void markMeters(Application& app) const;

        std::vector<LedgerEntry> getLiveEntries() const;
        std::vector<LedgerKey> getDeadEntries() const;


        xdr::msg_ptr getTransactionMeta() const;

    };
}
