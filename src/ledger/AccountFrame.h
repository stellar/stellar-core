#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"
#include <functional>
#include <map>
#include <unordered_map>

namespace soci
{
class session;
namespace details
{
class prepare_temp_type;
}
}

namespace stellar
{
class LedgerManager;
class LedgerRange;

class AccountFrame : public EntryFrame
{
    void storeUpdate(LedgerDelta& delta, Database& db, bool insert);
    bool mUpdateSigners;

    AccountEntry& mAccountEntry;

    void normalize();

    static std::vector<Signer> loadSigners(Database& db,
                                           std::string const& actIDStrKey);
    void applySigners(Database& db, bool insert);

  public:
    typedef std::shared_ptr<AccountFrame> pointer;

    AccountFrame();
    AccountFrame(LedgerEntry const& from);
    AccountFrame(AccountFrame const& from);
    AccountFrame(AccountID const& id);

    // builds an accountFrame for the sole purpose of authentication
    static AccountFrame::pointer makeAuthOnlyAccount(AccountID const& id);

    EntryFrame::pointer
    copy() const override
    {
        return std::make_shared<AccountFrame>(*this);
    }

    void
    setUpdateSigners()
    {
        normalize();
        mUpdateSigners = true;
    }

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
    AccountID const& getID() const;

    uint32_t getMasterWeight() const;
    uint32_t getHighThreshold() const;
    uint32_t getMediumThreshold() const;
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
    void storeChange(LedgerDelta& delta, Database& db) override;
    void storeAdd(LedgerDelta& delta, Database& db) override;

    // Static helper that don't assume an instance.
    static void storeDelete(LedgerDelta& delta, Database& db,
                            LedgerKey const& key);
    static bool exists(Database& db, LedgerKey const& key);
    static uint64_t countObjects(soci::session& sess);
    static uint64_t countObjects(soci::session& sess,
                                 LedgerRange const& ledgers);
    static void deleteAccountsModifiedOnOrAfterLedger(Database& db,
                                                      uint32_t oldestLedger);

    // database utilities
    static AccountFrame::pointer
    loadAccount(LedgerDelta& delta, AccountID const& accountID, Database& db);
    static AccountFrame::pointer loadAccount(AccountID const& accountID,
                                             Database& db);

    // compare signers, ignores weight
    static bool signerCompare(Signer const& s1, Signer const& s2);

    // inflation helper

    struct InflationVotes
    {
        int64 mVotes;
        AccountID mInflationDest;
    };

    // inflationProcessor returns true to continue processing, false otherwise
    static void processForInflation(
        std::function<bool(InflationVotes const&)> inflationProcessor,
        int maxWinners, Database& db);

    // loads all accounts from database and checks for consistency (slow!)
    static std::unordered_map<AccountID, AccountFrame::pointer>
    checkDB(Database& db);

    static void dropAll(Database& db);

  private:
    static const char* kSQLCreateStatement1;
    static const char* kSQLCreateStatement2;
    static const char* kSQLCreateStatement3;
    static const char* kSQLCreateStatement4;
};
}
