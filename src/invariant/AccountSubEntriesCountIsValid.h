#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include "ledger/LedgerDelta.h"
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
                          LedgerDelta const& delta) override;

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

    int32_t calculateDelta(LedgerEntry const* current,
                           LedgerEntry const* previous) const;

    void updateChangedSubEntriesCount(
        std::unordered_map<AccountID, SubEntriesChange>& subEntriesChange,
        LedgerEntry const* current, LedgerEntry const* previous) const;

    void countChangedSubEntries(
        std::unordered_map<AccountID, SubEntriesChange>& subEntriesChange,
        LedgerDelta::AddedIterator iter,
        LedgerDelta::AddedIterator const& end) const;
    void countChangedSubEntries(
        std::unordered_map<AccountID, SubEntriesChange>& subEntriesChange,
        LedgerDelta::ModifiedIterator iter,
        LedgerDelta::ModifiedIterator const& end) const;
    void countChangedSubEntries(
        std::unordered_map<AccountID, SubEntriesChange>& subEntriesChange,
        LedgerDelta::DeletedIterator iter,
        LedgerDelta::DeletedIterator const& end) const;
};
}
