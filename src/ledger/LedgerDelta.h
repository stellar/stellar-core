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

        LedgerDelta *mOuterDelta;
        LedgerHeader *mHeader;
        uint64_t mCurrentID;

        void checkState();
        void addEntry(EntryFrame::pointer entry);
        void deleteEntry(EntryFrame::pointer entry);
        void modEntry(EntryFrame::pointer entry);

        void merge(LedgerDelta &other);
    public:
        explicit LedgerDelta(LedgerDelta& outerDelta);
        LedgerDelta(LedgerHeader& ledgerHeader);

        void addEntry(EntryFrame const& entry);
        void deleteEntry(EntryFrame const& entry);
        void deleteEntry(LedgerKey const& key);
        void modEntry(EntryFrame const& entry);

        uint64_t getCurrentID() const  { return mCurrentID;  }
        uint64_t getNextID();

        // commits this delta into parent delta
        void commit();

        void markMeters(Application& app) const;

        std::vector<LedgerEntry> getLiveEntries() const;
        std::vector<LedgerKey> getDeadEntries() const;


        xdr::msg_ptr getTransactionMeta() const;

    };
}
