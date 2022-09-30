#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionMetaFrame.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

// Wrapper around TransactionResultSet XDR that mediates version differences.
class TransactionResultSetFrame
{
  public:
    TransactionResultSetFrame(uint32_t protocolVersion);

    void reserveResults(size_t);
    void pushMetaAndResultPair(TransactionMeta const& tm,
                               TransactionResultPair const& rp);
    TransactionResultSet const& getV1XDR();
    Hash getXDRHash();

  private:
    TransactionResultSet mTransactionResultSet;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    TransactionResultSetV2 mTransactionResultSetV2;
#endif

    int mVersion;
};

}
