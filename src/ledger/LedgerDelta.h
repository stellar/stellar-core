#include <map>
#include "ledger/LedgerEntry.h"

namespace stellar
{
    class LedgerDelta
    {
        std::map<stellarxdr::uint256, LedgerEntry::pointer> mNew;
        std::map<stellarxdr::uint256, LedgerEntry::pointer> mMod;
        std::map<stellarxdr::uint256, LedgerEntry::pointer> mDelete;
    public:

        void addEntry(LedgerEntry::pointer entry);
        void deleteEntry(LedgerEntry::pointer entry);
        void modEntry(LedgerEntry::pointer entry);

    };
    
    class TxDelta
    {
        std::map<stellarxdr::uint256, std::pair<LedgerEntry::pointer, LedgerEntry::pointer>> mStartEnd;
    public:
        void merge(const TxDelta& other);
        void setStart(const LedgerEntry& entry);
        void setFinal(const LedgerEntry& entry);

        void commitDelta(Json::Value& txResult, LedgerDelta& delta, LedgerMaster& ledgerMaster);
    };
}
