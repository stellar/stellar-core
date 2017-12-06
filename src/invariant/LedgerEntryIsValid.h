#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include "xdr/Stellar-ledger-entries.h"
#include <memory>

namespace stellar
{
class Application;
class LedgerDelta;
class LedgerManager;

// This Invariant is used to validate that LedgerEntries meet a number of simple
// requirements, such as bounds checking for a variety of fields. The Invariant
// also checks that the lastModifiedLedgerSeq equals the ledgerSeq of the
// LedgerHeader.
class LedgerEntryIsValid : public Invariant
{
  public:
    LedgerEntryIsValid();

    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerDelta const& delta) override;

  private:
    template <typename IterType>
    std::string check(IterType iter, IterType const& end,
                      uint32_t ledgerSeq) const;

    std::string checkIsValid(LedgerEntry const& le, uint32_t ledgerSeq) const;
    std::string checkIsValid(AccountEntry const& ae) const;
    std::string checkIsValid(TrustLineEntry const& tl) const;
    std::string checkIsValid(OfferEntry const& oe) const;
    std::string checkIsValid(DataEntry const& de) const;
};
}
