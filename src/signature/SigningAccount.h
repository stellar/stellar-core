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
    AccountID accountID;
    xdr::xvector<Signer, 20> signers {};
    uint32_t weight {1};
    uint32_t lowThreshold {0};
    uint32_t mediumThreshold {0};
    uint32_t highThreshold {0};

    SigningAccount() = default;
    explicit SigningAccount(AccountID accountID);
    explicit SigningAccount(AccountFrame const& accountFrame);
};

}
