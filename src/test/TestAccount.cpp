// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TestAccount.h"

#include "main/Application.h"
#include "test/TxTests.h"

namespace stellar
{

using namespace txtest;

TestAccount TestAccount::createRoot(Application& app)
{
    auto secretKey = getRoot(app.getNetworkID());
    auto sequenceNumber = getAccountSeqNum(secretKey, app);
    return TestAccount{app, secretKey, sequenceNumber};
}

TestAccount TestAccount::create(SecretKey const& secretKey, uint64_t initialBalance)
{
    applyCreateAccountTx(mApp, getSecretKey(), secretKey, nextSequenceNumber(), initialBalance);
    auto sequenceNumber = getAccountSeqNum(secretKey, mApp);
    return TestAccount{mApp, secretKey, sequenceNumber};
}

TestAccount TestAccount::create(std::string const& name, uint64_t initialBalance)
{
    return create(getAccount(name.c_str()), initialBalance);
}

};
