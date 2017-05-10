#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"

namespace stellar
{

class LedgerManager;

LedgerKey accountKey(AccountID accountID);

class AccountFrame : public EntryFrame
{
  public:
    AccountFrame(AccountID accountID, int64_t balance, SequenceNumber seqNum);
    AccountFrame(AccountID accountID);
    AccountFrame(AccountEntry account);
    AccountFrame(LedgerEntry entry);

    AccountID const& getAccountID() const;
    void setAccountID(AccountID accountID);

    void setHomeDomain(string32 homeDomain);

    // actual balance for the account
    int64_t getBalance() const;
    void setBalance(int64_t balance);

    // update balance for account
    bool addBalance(int64_t delta);

    // reserve balance that the account must always hold
    int64_t getMinimumBalance(LedgerManager const& lm);

    // balance that can be spent (above the limit)
    int64_t getBalanceAboveReserve(LedgerManager const& lm);

    int getNumSubEntries() const;

    xdr::xvector<Signer, 20> const& getSigners() const;
    void setSigners(xdr::xvector<Signer, 20> signers);
    int getNumSigners() const;

    // returns true if successfully updated,
    // false if balance is not sufficient
    bool addNumEntries(int count, LedgerManager const& lm);

    bool isAuthRequired();
    bool isRevocableAuth();
    bool isImmutableAuth();
    void setFlags(uint32_t flags);
    void clearFlags(uint32_t flags);

    xdr::pointer<AccountID> const& getInflationDest() const;
    void setInflationDest(AccountID inflationDest);

    uint32_t getMasterWeight() const;
    void setMasterWeight(uint32_t masterWeight);
    uint32_t getHighThreshold() const;
    void setHighThreshold(uint32_t highThreshold);
    uint32_t getMediumThreshold() const;
    void setMediumThreshold(uint32_t mediumThreshold);
    uint32_t getLowThreshold() const;
    void setLowThreshold(uint32_t lowThreshold);

    void setSeqNum(SequenceNumber seq);
    SequenceNumber getSeqNum() const;

    bool isValid();

    // compare signers, ignores weight
    static bool
    signerCompare(Signer const& s1, Signer const& s2)
    {
        using xdr::operator<;
        return s1.key < s2.key;
    }

    friend bool operator==(AccountFrame const& x, AccountFrame const& y);
    friend bool operator!=(AccountFrame const& x, AccountFrame const& y);
};
}
