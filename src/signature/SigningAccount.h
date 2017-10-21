#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

class AccountFrame;

struct SigningAccount
{
    AccountID mAccountID;
    xdr::xvector<Signer, 20> mSigners {};
    uint32_t mWeight {1};
    uint32_t mLowThreshold {0};
    uint32_t mMediumThreshold {0};
    uint32_t mHighThreshold {0};

    SigningAccount() = default;
    explicit SigningAccount(AccountID const& accountID);
    explicit SigningAccount(AccountFrame const& accountFrame);
};

}
