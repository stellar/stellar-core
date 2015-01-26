#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/EntryFrame.h"


namespace stellar
{
	class OfferFrame : public EntryFrame
	{
		void calculateIndex();
	public:

        enum OfferFlags
        {
            PASSIVE_FLAG = 1
        };

        OfferFrame();
        OfferFrame(const LedgerEntry& from);
        void from(const Transaction& tx);

        EntryFrame::pointer copy()  const { return EntryFrame::pointer(new OfferFrame(*this)); }

        void storeDelete(LedgerDelta &delta, LedgerMaster& ledgerMaster);
        void storeChange(LedgerDelta &delta, LedgerMaster& ledgerMaster);
        void storeAdd(LedgerDelta &delta, LedgerMaster& ledgerMaster);

        int64_t getPrice();
        int64_t getAmount();
        Currency& getTakerPays();
        Currency& getTakerGets();
        uint32 getSequence();

        static void dropAll(Database &db);
        static const char *kSQLCreateStatement;
	};
}


