#ifndef __BALLOT__
#define __BALLOT__

#include "txherder/TransactionSet.h"
#include "fba/Node.h"
#include "generated/stellar.hh"
#include "fba/FBA.h"

namespace stellar
{
	class Ledger;
	typedef std::shared_ptr<Ledger> LedgerPtr;

    namespace ballot
    {
        // returns true if b1 is ranked higher
        bool compare(const stellarxdr::Ballot& b1, const stellarxdr::Ballot& b2);
        bool compareValue(const stellarxdr::Ballot& b1, const stellarxdr::Ballot& b2);
        bool isCompatible(const stellarxdr::Ballot& b1, const stellarxdr::Ballot& b2);
    }
}

#endif
