#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger.h"
#include <memory>

namespace stellar
{

class OperationMetaArray
{
  private:
    struct OpMetaInner
    {
        LedgerEntryChanges mLeChanges;
        xdr::xvector<ContractEvent> mContractEvents;
        // OperationMeta doesn't store diagnostic events, those go at the
        // transaction level. Because we want to retain the diagnostic events
        // even if the operation fails (thus no operation meta is emitted)

        OpMetaInner(LedgerEntryChanges&& lec, xdr::xvector<ContractEvent>&& ces)
            : mLeChanges(lec), mContractEvents(ces)
        {
        }
    };

    std::vector<OpMetaInner> mInner;

  public:
    OperationMetaArray(uint32_t reserveSize);

    void push(LedgerEntryChanges&& lec, xdr::xvector<ContractEvent>&& ces);

    xdr::xvector<stellar::OperationMeta> convertToXDR();

    xdr::xvector<stellar::OperationMetaV2> convertToXDRV2();

    xdr::xvector<ContractEvent> flushContractEvents();
};

}