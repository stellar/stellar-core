// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/TxResultCode.h"
#include "TrustSetFrame.h"
#include "ledger/TrustFrame.h"
#include "ledger/LedgerMaster.h"

namespace stellar
{
    void TrustSetFrame::fillTrustLine(TrustFrame& line)
    {
        line.mEntry.trustLine().limit
        line.mEntry.trustLine().authorized;
    }

    // see if we are modifying an old trustline
    void TrustSetFrame::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        TrustFrame trustLine;
        if(ledgerMaster.getDatabase().loadTrustLine(mSigningAccount.mEntry.account().accountID,
            mEnvelope.tx.body.changeTrustTx().line, trustLine))
        {
            delta.setStart(trustLine);
            fillTrustLine(trustLine);
            delta.setFinal(trustLine);
        } else
        { // new trust line
            trustLine.mEntry.type(TRUSTLINE);
            trustLine.mEntry.trustLine().accountID = mSigningAccount.mEntry.account().accountID;
            fillTrustLine(trustLine);

            delta.setFinal(trustLine);
        }
    }

    /* NICOLAS
	TxResultCode TrustSetTx::doApply()
	{
		TrustLine txTrustLine;
		TxResultCode terResult = txTrustLine.fromTx(mSigningAccount, this);
		if(terResult == txSUCCESS)
		{
			// NICOLAS anything else to check here?

			bool modSigner = false;

			// Pull this trust line from SQL
			// change this side of the line
			// save the line back to SQL
			TrustLine existingLine;
			if(existingLine.loadFromDB(txTrustLine.getIndex()))
			{ // modify existing trustline
				bool lowerOwnerCount = false;
				bool raiseOwnerCount = false;
				if(mSigningAccount.mAccountID == existingLine.mLowAccount)
				{	// we are moding the lowAccount
					if(existingLine.mLowLimit > zero && txTrustLine.mLowLimit == zero && existingLine.mBalance >= zero) lowerOwnerCount = true;
					else if(existingLine.mLowLimit == zero && txTrustLine.mLowLimit > zero && existingLine.mBalance <= zero) raiseOwnerCount = true;
					existingLine.mLowLimit = txTrustLine.mLowLimit;
					existingLine.mHighAuthSet = txTrustLine.mHighAuthSet;

				} else
				{	// we are moding the highAccount
					if(existingLine.mHighLimit > zero && txTrustLine.mHighLimit == zero && existingLine.mBalance <= zero) lowerOwnerCount = true;
					else if(existingLine.mHighLimit == zero && txTrustLine.mHighLimit > zero && existingLine.mBalance >= zero) raiseOwnerCount = true;
					existingLine.mHighLimit = txTrustLine.mHighLimit;
					existingLine.mLowAuthSet = txTrustLine.mLowAuthSet;
				}

				if(lowerOwnerCount)
				{
					mSigningAccount.mOwnerCount--;
				} else if(raiseOwnerCount)
				{
					// make sure the account has enough reserve
					terResult = mSigningAccount.tryToIncreaseOwnerCount();
					if(terResult != txSUCCESS) return(terResult);
				}
				//check if line should be deleted
				if(existingLine.mLowLimit == zero && existingLine.mHighLimit == zero && existingLine.mBalance == zero)
				{	// delete line
					existingLine.storeDelete();
				} else
				{
					existingLine.storeChange();
				}

				modSigner = lowerOwnerCount | raiseOwnerCount;

			} else
			{  // this trust line doesn't exist yet

				// make sure the account has enough reserve
				terResult = mSigningAccount.tryToIncreaseOwnerCount();
				if(terResult != txSUCCESS) return(terResult);

				modSigner = true;

				existingLine.storeAdd();
			}

			if(modSigner)
			{
				mSigningAccount.storeChange();
			}
		} else
		{	// some error building trustline from tx
			return(terResult);
		}

		return terResult;
	}
    */
}

