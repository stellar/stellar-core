#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerEntryReference.h"
#include "xdr/Stellar-ledger-entries.h"
#include <memory>

namespace stellar
{

class LedgerManager;

class AccountReference
{
    std::shared_ptr<LedgerEntryReference> mEntry;

  public:
    AccountReference(std::shared_ptr<LedgerEntryReference> const& ler);

    AccountEntry& account();
    AccountEntry const& account() const;

    AccountID const& getID() const;

    // actual balance for the account
    int64_t getBalance() const;

    // update balance for account
    bool addBalance(int64_t delta);

    // reserve balance that the account must always hold
    int64_t getMinimumBalance(LedgerManager const& lm) const;

    // balance that can be spent (above the limit)
    int64_t getBalanceAboveReserve(LedgerManager const& lm) const;

    // returns true if successfully updated,
    // false if balance is not sufficient
    bool addNumEntries(int count, LedgerManager const& lm);

    bool isAuthRequired() const;
    bool isImmutableAuth() const;

    uint32_t getMasterWeight() const;
    uint32_t getHighThreshold() const;
    uint32_t getMediumThreshold() const;
    uint32_t getLowThreshold() const;

    // TODO(jonjove): Change uses of setUpdateSigners to normalizeSigners
    void normalizeSigners();

    void setSeqNum(SequenceNumber seq);

    SequenceNumber getSeqNum() const;

    // compare signers, ignores weight
    static bool signerCompare(Signer const& s1, Signer const& s2);
};
}
