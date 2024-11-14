// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionFrameBase.h"
#include "ledger/LedgerManager.h"
#include "main/AppConnector.h"
#include "transactions/FeeBumpTransactionFrame.h"
#include "transactions/TransactionFrame.h"

namespace stellar
{

AppValidationWrapper::AppValidationWrapper(AppConnector const& app,
                                           bool forApply)
    : mApp(app), mForApply(forApply)
{
}

Config const&
AppValidationWrapper::getConfig() const
{
    return mApp.getConfig();
}

SorobanNetworkConfig const&
AppValidationWrapper::getSorobanNetworkConfig() const
{
    return mForApply ? mApp.getLedgerManager().getSorobanNetworkConfigForApply()
                     : mApp.getSorobanNetworkConfigReadOnly();
}

uint32_t
AppValidationWrapper::getCurrentProtocolVersion() const
{
    return mApp.getLedgerManager()
        .getLastClosedLedgerHeader()
        .header.ledgerVersion;
}

ImmutableValidationSnapshot::ImmutableValidationSnapshot(
    AppConnector const& app)
    : mConfig(app.getConfigPtr())
    , mSorobanNetworkConfig(app.maybeGetSorobanNetworkConfigReadOnly())
    , mCurrentProtocolVersion(app.getLedgerManager()
                                  .getLastClosedLedgerHeader()
                                  .header.ledgerVersion)
{
    releaseAssert(threadIsMain());
}

Config const&
ImmutableValidationSnapshot::getConfig() const
{
    return *mConfig;
}

SorobanNetworkConfig const&
ImmutableValidationSnapshot::getSorobanNetworkConfig() const
{
    // TODO: This can throw. Check and throw a more usefull exception instead.
    // Also document this.
    return mSorobanNetworkConfig.value();
}

uint32_t
ImmutableValidationSnapshot::getCurrentProtocolVersion() const
{
    return mCurrentProtocolVersion;
}

TransactionFrameBasePtr
TransactionFrameBase::makeTransactionFromWire(Hash const& networkID,
                                              TransactionEnvelope const& env)
{
    switch (env.type())
    {
    case ENVELOPE_TYPE_TX_V0:
    case ENVELOPE_TYPE_TX:
        return std::make_shared<TransactionFrame>(networkID, env);
    case ENVELOPE_TYPE_TX_FEE_BUMP:
        return std::make_shared<FeeBumpTransactionFrame>(networkID, env);
    default:
        abort();
    }
}
}
