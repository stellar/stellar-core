#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include "xdr/Stellar-ledger-entries.h"
#include <memory>
#include <unordered_map>

namespace stellar
{

class Application;
class Database;

// This Invariant is used to validate that the numSubEntries field of an
// account is in sync with the number of subentries in the database.
class AccountSubEntriesCountIsValid : public Invariant
{
  public:
    AccountSubEntriesCountIsValid();

    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerState& ls) override;

  private:
    struct SubEntriesChange
    {
        int32_t numSubEntries;
        int32_t calculatedSubEntries;
        int32_t signers;

        SubEntriesChange()
            : numSubEntries(0), calculatedSubEntries(0), signers(0)
        {
        }
    };

    int32_t calculateDelta(std::shared_ptr<LedgerEntry const> const& current,
                           std::shared_ptr<LedgerEntry const> const& previous) const;

    void updateChangedSubEntriesCount(
        std::unordered_map<AccountID, SubEntriesChange>& subEntriesChange,
        std::shared_ptr<LedgerEntry const> const& current,
        std::shared_ptr<LedgerEntry const> const& previous) const;

    void countChangedSubEntries(
        std::unordered_map<AccountID, SubEntriesChange>& subEntriesChange,
        LedgerState const& ls) const;
};
}
