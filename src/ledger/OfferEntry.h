#ifndef __OFFERENTRY__
#define __OFFERENTRY__

#include "LedgerEntry.h"


namespace stellar
{
	class OfferEntry : public LedgerEntry
	{
		void insertIntoDB();
		void updateInDB();
		void deleteFromDB();

		void calculateIndex();
	public:
		stellarxdr::uint160 mAccountID;
		uint32_t	mSequence;
		//STAmount mTakerPays;
		//STAmount mTakerGets;
		bool mPassive;
		uint32_t mExpiration;


 		OfferEntry();
		//OfferEntry(SLE::pointer sle);

        static void dropAll(LedgerDatabase &db);
        static const char *kSQLCreateStatement;
	};
}

#endif