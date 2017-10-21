// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "signature/SigningAccount.h"
#include "ledger/AccountFrame.h"

namespace stellar
{

SigningAccount::SigningAccount(AccountID const& accountID) :
    mAccountID{accountID}
{
}

SigningAccount::SigningAccount(AccountFrame const& accountFrame) :
    mAccountID{accountFrame.getAccount().accountID},
    mSigners{accountFrame.getAccount().signers},
    mWeight{accountFrame.getMasterWeight()},
    mLowThreshold{accountFrame.getLowThreshold()},
    mMediumThreshold{accountFrame.getMediumThreshold()},
    mHighThreshold{accountFrame.getHighThreshold()}
{
}

}
