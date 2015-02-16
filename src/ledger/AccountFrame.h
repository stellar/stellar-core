#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/EntryFrame.h"
#include <functional>

namespace soci
{
    namespace details
    {
        class prepare_temp_type;
    }
}

namespace stellar
{
	class AccountFrame : public EntryFrame
	{
        void storeUpdate(LedgerDelta &delta, Database& db, bool insert);
        bool mUpdateSigners;

        AccountEntry &mAccountEntry;
	public:
        typedef std::shared_ptr<AccountFrame> pointer;

        AccountFrame();
        AccountFrame(LedgerEntry const& from);
        AccountFrame(uint256 const& id);
        AccountFrame(AccountFrame const &from);

        EntryFrame::pointer copy()  const  { return EntryFrame::pointer(new AccountFrame(*this)); }

        void setUpdateSigners() { mUpdateSigners = true; }
        int64_t getBalance();
        bool isAuthRequired();
        uint256 const& getID() const;
        uint32_t getMasterWeight();
        uint32_t getHighThreshold();
        uint32_t getMidThreshold();
        uint32_t getLowThreshold();
        uint32_t getSeqNum();
        xdr::xvector<Signer> &getSigners();

        AccountEntry &getAccount() { return mAccountEntry; }

        // Instance-based overrides of EntryFrame.
        void storeDelete(LedgerDelta &delta, Database& db) override;
        void storeChange(LedgerDelta &delta, Database& db) override;
        void storeAdd(LedgerDelta &delta, Database& db) override;

        // Static helper that doesn't assume an instance.
        static void storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key);

        // database utilities
        static bool loadAccount(const uint256& accountID, AccountFrame& retEntry,
            Database& db, bool withSig = false);
        static void dropAll(Database &db);
        static const char *kSQLCreateStatement1;
        static const char *kSQLCreateStatement2;
        static const char *kSQLCreateStatement3;
	};
}



