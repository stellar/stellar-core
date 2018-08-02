#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include "ledger/LedgerDelta.h"
#include <memory>

namespace stellar
{

class Application;
class LedgerManager;

// This Invariant has two purposes: to ensure that liabilities remain in sync
// with the offer book, and to ensure that the balance of accounts and
// trustlines respect the liabilities (and reserve). It is important to note
// that accounts can be below the minimum balance if the minimum balance
// increased since the last time the balance of those accounts decreased.
// Therefore, the Invariant only checks accounts that have had their balance
// decrease or their liabilities increase in the operation.
class LiabilitiesMatchOffers : public Invariant
{
  public:
    explicit LiabilitiesMatchOffers(LedgerManager& lm);

    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerDelta const& delta) override;

  private:
    template <typename IterType>
    void addCurrentLiabilities(
        std::map<AccountID, std::map<Asset, Liabilities>>& deltaLiabilities,
        IterType const& iter) const;

    template <typename IterType>
    void subtractPreviousLiabilities(
        std::map<AccountID, std::map<Asset, Liabilities>>& deltaLiabilities,
        IterType const& iter) const;

    bool shouldCheckAccount(LedgerDelta::AddedLedgerEntry const& ale,
                            uint32_t ledgerVersion) const;
    bool shouldCheckAccount(LedgerDelta::ModifiedLedgerEntry const& ale,
                            uint32_t ledgerVersion) const;

    template <typename IterType>
    std::string checkAuthorized(IterType const& iter) const;

    template <typename IterType>
    std::string checkBalanceAndLimit(IterType const& iter,
                                     uint32_t ledgerVersion) const;

    LedgerManager& mLedgerManager;
};
}
