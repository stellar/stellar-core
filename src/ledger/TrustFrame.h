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

namespace stellar {

	class TrustSetTx;
	
    class TrustFrame : public EntryFrame
    {
        void getKeyFields(std::string& base58AccountID,
                          std::string& base58Issuer,
                          std::string& currencyCode) const;

        static void loadLines(soci::details::prepare_temp_type &prep,
            std::function<void(const TrustFrame&)> trustProcessor);

        TrustLineEntry &mTrustLine;
	public:
        typedef std::shared_ptr<TrustFrame> pointer;

        TrustFrame();
        TrustFrame(LedgerEntry const& from);
        TrustFrame(TrustFrame const& from);

        TrustFrame& operator=(TrustFrame const& other);

        EntryFrame::pointer copy()  const { return EntryFrame::pointer(new TrustFrame(*this)); }

        // Instance-based overrides of EntryFrame.
        void storeDelete(LedgerDelta &delta, Database& db) override;
        void storeChange(LedgerDelta &delta, Database& db) override;
        void storeAdd(LedgerDelta &delta, Database& db) override;

        // Static helper that doesn't assume an instance.
        static void storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key);

        static bool loadTrustLine(const uint256& accountID, const Currency& currency,
            TrustFrame& retEntry, Database& db);

        static void loadLines(const uint256& accountID,
            std::vector<TrustFrame>& retLines, Database& db);

        int64_t getBalance();
        TrustLineEntry& getTrustLine() { return mTrustLine; }

        bool isValid() const;

        static void dropAll(Database &db);
        static const char *kSQLCreateStatement;
	};
}


