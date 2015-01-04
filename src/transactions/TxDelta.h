#ifndef __TXDELTA__
#define __TXDELTA__

#include <map>
#include "generated/StellarXDR.h"
#include "ledger/LedgerDelta.h"

namespace stellar
{
class TxDelta
{
    typedef std::pair<EntryFrame::pointer, EntryFrame::pointer> StartEndPair;
    std::map<uint256, StartEndPair> mStartEnd;
public:
    void merge(const TxDelta& other);
    void setStart(EntryFrame& entry);
    void setFinal(EntryFrame& entry);
    void removeFinal(EntryFrame& entry);

    void commitDelta(Json::Value& txResult, LedgerDelta& delta, LedgerMaster& ledgerMaster);

    static void dropAll(Database& db);
    static const char *kSQLCreateStatement;
};
}

#endif

