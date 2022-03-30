// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerCloseMetaFrame.h"
#include "util/ProtocolVersion.h"

namespace stellar
{
LedgerCloseMetaFrame::LedgerCloseMetaFrame(uint32_t protocolVersion)
{
    mVersion = protocolVersionIsBefore(protocolVersion,
                                       GENERALIZED_TX_SET_PROTOCOL_VERSION)
                   ? 0
                   : 1;
    mLedgerCloseMeta.v(mVersion);
}

LedgerHeaderHistoryEntry&
LedgerCloseMetaFrame::ledgerHeader()
{
    if (mVersion == 0)
    {
        return mLedgerCloseMeta.v0().ledgerHeader;
    }
    return mLedgerCloseMeta.v1().ledgerHeader;
}

xdr::xvector<TransactionResultMeta>&
LedgerCloseMetaFrame::txProcessing()
{
    if (mVersion == 0)
    {
        return mLedgerCloseMeta.v0().txProcessing;
    }
    return mLedgerCloseMeta.v1().txProcessing;
}

xdr::xvector<UpgradeEntryMeta>&
LedgerCloseMetaFrame::upgradesProcessing()
{
    if (mVersion == 0)
    {
        return mLedgerCloseMeta.v0().upgradesProcessing;
    }
    return mLedgerCloseMeta.v1().upgradesProcessing;
}

xdr::xvector<SCPHistoryEntry>&
LedgerCloseMetaFrame::scpInfo()
{
    if (mVersion == 0)
    {
        return mLedgerCloseMeta.v0().scpInfo;
    }
    return mLedgerCloseMeta.v1().scpInfo;
}

void
LedgerCloseMetaFrame::populateTxSet(AbstractTxSetFrameForApply const& txSet)
{
    if (mVersion == 0)
    {
        txSet.toXDR(mLedgerCloseMeta.v0().txSet);
        return;
    }
    txSet.toXDR(mLedgerCloseMeta.v1().txSet);
}

LedgerCloseMeta const&
LedgerCloseMetaFrame::getXDR() const
{
    return mLedgerCloseMeta;
}
}
