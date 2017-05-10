// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "signature/SigningAccount.h"
#include "ledger/AccountFrame.h"

namespace stellar
{

SigningAccount::SigningAccount(AccountID accountID) :
    accountID{std::move(accountID)}
{
}

SigningAccount::SigningAccount(AccountFrame const& accountFrame) :
    accountID{accountFrame.getAccount().accountID},
    signers{accountFrame.getAccount().signers},
    weight{accountFrame.getMasterWeight()},
    lowThreshold{accountFrame.getLowThreshold()},
    mediumThreshold{accountFrame.getMediumThreshold()},
    highThreshold{accountFrame.getHighThreshold()}
{
}

}
