// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "AccountFrame.h"
#include "LedgerMaster.h"
#include "lib/json/json.h"
#include "crypto/Base58.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "LedgerDelta.h"

using namespace soci;
using namespace std;

namespace stellar
{
const char *AccountFrame::kSQLCreateStatement1 = "CREATE TABLE IF NOT EXISTS Accounts (						\
	accountID		CHARACTER(35) PRIMARY KEY,	\
	balance			BIGINT UNSIGNED,			\
	sequence		INT UNSIGNED default 1,		\
	ownerCount		INT UNSIGNED default 0,		\
	transferRate	INT UNSIGNED default 0,		\
    inflationDest	CHARACTER(35),		        \
    thresholds   	BLOB(4),		            \
	flags		    INT UNSIGNED default 0  	\
);";

const char *AccountFrame::kSQLCreateStatement2 = "CREATE TABLE IF NOT EXISTS Signers (						\
	accountID		CHARACTER(35) PRIMARY KEY,	\
    publicKey   	CHARACTER(35),		        \
    weight	INT 	\
);";

const char *AccountFrame::kSQLCreateStatement3 = "CREATE TABLE IF NOT EXISTS AccountData (						\
	accountID		CHARACTER(35) PRIMARY KEY,	\
    key INT, \
	value			BLOB(32)			\
);";

AccountFrame::AccountFrame()
{
    mEntry.type(ACCOUNT);
    mEntry.account().sequence = 1;
    mEntry.account().transferRate = TRANSFER_RATE_DIVISOR;
    mEntry.account().thresholds[0] = 1; // by default, master key's weight is 1
    mUpdateSigners = false;
}

AccountFrame::AccountFrame(LedgerEntry const& from) : EntryFrame(from)
{
    mUpdateSigners = false;
}

AccountFrame::AccountFrame(uint256 const& id)
{
    mEntry.type(ACCOUNT);
    mEntry.account().accountID = id;
    mEntry.account().transferRate = TRANSFER_RATE_DIVISOR;
    mEntry.account().sequence = 1;
    mEntry.account().thresholds[0] = 1; // by default, master key's weight is 1
    mUpdateSigners = false;
}

void AccountFrame::calculateIndex()
{
    mIndex = mEntry.account().accountID;
}

bool AccountFrame::isAuthRequired()
{
    return(mEntry.account().flags & AccountFrame::AUTH_REQUIRED_FLAG);
}

uint32_t AccountFrame::getSeqNum()
{
    return(mEntry.account().sequence);
}

uint64_t AccountFrame::getBalance()
{
    return(mEntry.account().balance);
}
uint256& AccountFrame::getID()
{
    return(mEntry.account().accountID);
}
uint32_t AccountFrame::getMasterWeight()
{
    return mEntry.account().thresholds[0];
}
uint32_t AccountFrame::getHighThreshold()
{
    return mEntry.account().thresholds[3];
}
uint32_t AccountFrame::getMidThreshold()
{
    return mEntry.account().thresholds[2];
}
uint32_t AccountFrame::getLowThreshold()
{
    return mEntry.account().thresholds[1];
}

void AccountFrame::storeDelete(LedgerDelta &delta, LedgerMaster& ledgerMaster)
{
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

    ledgerMaster.getDatabase().getSession() << 
        "DELETE from Accounts where accountID= :v1", soci::use(base58ID);
    ledgerMaster.getDatabase().getSession() <<
        "DELETE from AccountData where accountID= :v1", soci::use(base58ID);
    ledgerMaster.getDatabase().getSession() <<
        "DELETE from Signers where accountID= :v1", soci::use(base58ID);

    delta.deleteEntry(*this);
}

void AccountFrame::storeUpdate(LedgerDelta &delta, LedgerMaster& ledgerMaster, bool insert)
{
    AccountEntry& finalAccount = mEntry.account();
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

    std::stringstream sql;

    const char * op = insert ? "new" : "mod";

    if (insert)
    {
        sql << "INSERT INTO Accounts ( accountID, balance, sequence,    \
            ownerCount, transferRate, inflationDest, thresholds, flags) \
            VALUES ( :id, :v1, :v2, :v3, :v4, :v5, :v6, :v7 )";
    }
    else
    {
        sql << "UPDATE Accounts SET balance = :v1, sequence = :v2, ownerCount = :v3, \
                transferRate = :v4, inflationDest = :v5, thresholds = :v6, \
                flags = :v7 WHERE accountID = :id";
    }

    soci::indicator inflation_ind = soci::i_null;
    string inflationDestStr;

    if(finalAccount.inflationDest)
    {
        inflationDestStr = toBase58Check(VER_ACCOUNT_PUBLIC, *finalAccount.inflationDest);
        inflation_ind = soci::i_ok;
    }

    // TODO.3   KeyValue data

    string thresholds(binToHex(finalAccount.thresholds));

    {
        soci::statement st = (ledgerMaster.getDatabase().getSession().prepare <<
            sql.str(), use(base58ID, "id"),
            use(finalAccount.balance, "v1"), use(finalAccount.sequence, "v2"),
            use(finalAccount.ownerCount, "v3"), use(finalAccount.transferRate, "v4"),
            use(inflationDestStr, inflation_ind, "v5"),
            use(thresholds, "v6"), use(finalAccount.flags, "v7"));

        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
        if (insert)
        {
            delta.addEntry(*this);
        }
        else
        {
            delta.modEntry(*this);
        }
    }

    if (mUpdateSigners)
    {
        // TODO: don't do this
        // instead separate signatures from account, just like offers are separate entities
        AccountFrame startAccountFrame;
        if (!ledgerMaster.getDatabase().loadAccount(getID(), startAccountFrame, true))
        {
            throw runtime_error("could not load account!");
        }
        AccountEntry &startAccount = startAccountFrame.mEntry.account();

        // deal with changes to Signers
        if (finalAccount.signers.size() < startAccount.signers.size())
        { // some signers were removed
            for (auto startSigner : startAccount.signers)
            {
                bool found = false;
                for (auto finalSigner : finalAccount.signers)
                {
                    if (finalSigner.pubKey == startSigner.pubKey)
                    {
                        if (finalSigner.weight != startSigner.weight)
                        {
                            std::string b58signKey = toBase58Check(VER_ACCOUNT_ID, finalSigner.pubKey);
                            ledgerMaster.getDatabase().getSession() << "UPDATE Signers set weight=:v1 where accountID=:v2 and pubKey=:v3",
                                use(finalSigner.weight), use(base58ID), use(b58signKey);
                        }
                        found = true;
                        break;
                    }
                }
                if (!found)
                { // delete signer
                    std::string b58signKey = toBase58Check(VER_ACCOUNT_ID, startSigner.pubKey);

                    soci::statement st = (ledgerMaster.getDatabase().getSession().prepare <<
                        "DELETE from Signers where accountID=:v2 and pubKey=:v3",
                        use(base58ID), use(b58signKey));

                    st.execute(true);

                    if (st.get_affected_rows() != 1)
                    {
                        throw std::runtime_error("Could not update data in SQL");
                    }
                }
            }
        }
        else
        { // signers added or the same
            for (auto finalSigner : finalAccount.signers)
            {
                bool found = false;
                for (auto startSigner : startAccount.signers)
                {
                    if (finalSigner.pubKey == startSigner.pubKey)
                    {
                        if (finalSigner.weight != startSigner.weight)
                        {
                            std::string b58signKey = toBase58Check(VER_ACCOUNT_ID, finalSigner.pubKey);

                            soci::statement st = (ledgerMaster.getDatabase().getSession().prepare <<
                                "UPDATE Signers set weight=:v1 where accountID=:v2 and pubKey=:v3",
                                use(finalSigner.weight), use(base58ID), use(b58signKey));

                            st.execute(true);

                            if (st.get_affected_rows() != 1)
                            {
                                throw std::runtime_error("Could not update data in SQL");
                            }
                        }
                        found = true;
                        break;
                    }
                }
                if (!found)
                { // new signer
                    std::string b58signKey = toBase58Check(VER_ACCOUNT_ID, finalSigner.pubKey);

                    soci::statement st = (ledgerMaster.getDatabase().getSession().prepare <<
                        "INSERT INTO Signers (accountID,pubKey,weight) values (:v1,:v2,:v3)",
                        use(base58ID), use(b58signKey), use(finalSigner.weight));

                    st.execute(true);

                    if (st.get_affected_rows() != 1)
                    {
                        throw std::runtime_error("Could not update data in SQL");
                    }
                }
            }
        }
    }
}

void AccountFrame::storeChange(LedgerDelta &delta, LedgerMaster& ledgerMaster)
{
    storeUpdate(delta, ledgerMaster, false);
}

void AccountFrame::storeAdd(LedgerDelta &delta, LedgerMaster& ledgerMaster)
{
    EntryFrame::pointer emptyAccount = make_shared<AccountFrame>(mEntry.account().accountID);
    storeUpdate(delta, ledgerMaster, true);
}

void AccountFrame::dropAll(Database &db)
{
    db.getSession() << "DROP TABLE IF EXISTS Accounts;";
    db.getSession() << "DROP TABLE IF EXISTS Signers;";
    db.getSession() << "DROP TABLE IF EXISTS AccountData;";

    db.getSession() << kSQLCreateStatement1;
    db.getSession() << kSQLCreateStatement2;
    db.getSession() << kSQLCreateStatement3;
}
}

