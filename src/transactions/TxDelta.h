#ifndef __TXDELTA__
#define __TXDELTA__

#include <map>
#include "generated/StellarXDR.h"
#include "ledger/LedgerEntry.h"
#include "ledger/LedgerDelta.h"

namespace stellar
{
class TxDelta
{
    std::map<stellarxdr::uint256, std::pair<LedgerEntry::pointer, LedgerEntry::pointer>> mStartEnd;
public:
    void merge(const TxDelta& other);
    void setStart(LedgerEntry& entry);
    void setFinal(LedgerEntry& entry);

    void commitDelta(Json::Value& txResult, LedgerDelta& delta, LedgerMaster& ledgerMaster);

    static const char *kSQLCreateStatement;
};
}

#endif

