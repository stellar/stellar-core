#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/EntryFrame.h"
#include <functional>
#include <map>

namespace soci
{
namespace details
{
class prepare_temp_type;
}
}

namespace stellar
{
class LedgerManager;

class AccountFrame : public EntryFrame
{
    void storeUpdate(LedgerDelta& delta, Database& db, bool insert) const;
    bool mUpdateSigners;

    AccountEntry& mAccountEntry;

    void normalize();

  public:
    typedef std::shared_ptr<AccountFrame> pointer;

    AccountFrame();
    AccountFrame(LedgerEntry const& from);
    AccountFrame(uint256 const& id);
    AccountFrame(AccountFrame const& from);

    EntryFrame::pointer
    copy() const
    {
        return EntryFrame::pointer(new AccountFrame(*this));
    }

    void
    setUpdateSigners()
    {
        normalize();
        mUpdateSigners = true;
    }

    // actual balance for the account
    int64_t getBalance() const;

    // reserve balance that the account must always hold
    int64_t getMinimumBalance(LedgerManager const& lm) const;

    // balance that can be spent (above the limit)
    int64_t getBalanceAboveReserve(LedgerManager const& lm) const;

    // returns true if successfully updated,
    // false if balance is not sufficient
    bool addNumEntries(int count, LedgerManager const& lm);

    bool isAuthRequired() const;
    uint256 const& getID() const;

    uint32_t getMasterWeight() const;
    uint32_t getHighThreshold() const;
    uint32_t getMidThreshold() const;
    uint32_t getLowThreshold() const;

    void
    setSeqNum(SequenceNumber seq)
    {
        clearCached();
        mAccountEntry.seqNum = seq;
    }
    SequenceNumber
    getSeqNum() const
    {
        return mAccountEntry.seqNum;
    }

    AccountEntry const&
    getAccount() const
    {
        return mAccountEntry;
    }

    AccountEntry&
    getAccount()
    {
        clearCached();
        return mAccountEntry;
    }

    // Instance-based overrides of EntryFrame.
    void storeDelete(LedgerDelta& delta, Database& db) const override;
    void storeChange(LedgerDelta& delta, Database& db) const override;
    void storeAdd(LedgerDelta& delta, Database& db) const override;

    // Static helper that don't assume an instance.
    static void storeDelete(LedgerDelta& delta, Database& db,
                            LedgerKey const& key);
    static bool exists(Database& db, LedgerKey const& key);

    // database utilities
    static bool loadAccount(const uint256& accountID, AccountFrame& retEntry,
                            Database& db);
    static void dropAll(Database& db);
    static const char* kSQLCreateStatement1;
    static const char* kSQLCreateStatement2;
};
}
