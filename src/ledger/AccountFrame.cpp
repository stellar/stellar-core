// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "AccountFrame.h"
#include "lib/json/json.h"
#include "crypto/Base58.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "LedgerDelta.h"
#include "ledger/LedgerMaster.h"

using namespace soci;
using namespace std;


namespace stellar
{
const char *AccountFrame::kSQLCreateStatement1 =
    "CREATE TABLE Accounts"
     "("
     "accountID       VARCHAR(51)  PRIMARY KEY,"
     "balance         BIGINT       NOT NULL,"
     "seqNum          INT          NOT NULL,"
     "numSubEntries   INT          NOT NULL CHECK (numSubEntries >= 0),"
     "inflationDest   VARCHAR(51),"
     "thresholds      TEXT,"
     "flags           INT          NOT NULL"
     ");";

const char *AccountFrame::kSQLCreateStatement2 =
    "CREATE TABLE Signers"
     "("
     "accountID       VARCHAR(51) NOT NULL,"
     "publicKey       VARCHAR(51) NOT NULL,"
     "weight          INT         NOT NULL,"
     "PRIMARY KEY (accountID, publicKey)"
     ");";

AccountFrame::AccountFrame() : EntryFrame(ACCOUNT), mAccountEntry(mEntry.account())
{
    mAccountEntry.thresholds[0] = 1; // by default, master key's weight is 1
    mUpdateSigners = false;
}

AccountFrame::AccountFrame(LedgerEntry const& from) : EntryFrame(from), mAccountEntry(mEntry.account())
{
    mUpdateSigners = false;
}

AccountFrame::AccountFrame(AccountFrame const &from) : AccountFrame(from.mEntry)
{
}

AccountFrame::AccountFrame(uint256 const& id) : AccountFrame()
{
    mAccountEntry.accountID = id;
}

bool AccountFrame::isAuthRequired()
{
    return(mAccountEntry.flags & AUTH_REQUIRED_FLAG);
}


int64_t AccountFrame::getBalance()
{
    return(mAccountEntry.balance);
}

int64_t AccountFrame::getMinimumBalance(LedgerMaster const& lm) const
{
    return lm.getMinBalance(mAccountEntry.numSubEntries);
}

uint256 const& AccountFrame::getID() const
{
    return(mAccountEntry.accountID);
}

uint32_t AccountFrame::getMasterWeight()
{
    return mAccountEntry.thresholds[0];
}

uint32_t AccountFrame::getHighThreshold()
{
    return mAccountEntry.thresholds[3];
}

uint32_t AccountFrame::getMidThreshold()
{
    return mAccountEntry.thresholds[2];
}

uint32_t AccountFrame::getLowThreshold()
{
    return mAccountEntry.thresholds[1];
}

bool AccountFrame::loadAccount(const uint256& accountID, AccountFrame& retAcc,
    Database &db, bool withSig)
{
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, accountID);
    std::string publicKey, inflationDest, creditAuthKey;
    std::string thresholds;
    soci::indicator inflationDestInd, thresholdsInd;

    soci::session &session = db.getSession();

    retAcc.getAccount().accountID = accountID;
    AccountEntry& account = retAcc.getAccount();
    {
        auto timer = db.getSelectTimer("account");
        session << "SELECT balance, seqNum, numSubEntries, \
            inflationDest, thresholds,  flags from Accounts where accountID=:v1",
            into(account.balance), into(account.seqNum),into(account.numSubEntries),
            into(inflationDest, inflationDestInd),
            into(thresholds, thresholdsInd), into(account.flags),
            use(base58ID);
    }


    if (!session.got_data())
        return false;

    if (thresholdsInd == soci::i_ok)
    {
        std::vector<uint8_t> bin = hexToBin(thresholds);
        for (size_t n = 0; (n < 4) && (n < bin.size()); n++)
        {
            retAcc.mAccountEntry.thresholds[n] = bin[n];
        }
    }

    if (inflationDestInd == soci::i_ok)
    {
        account.inflationDest.activate() = fromBase58Check256(VER_ACCOUNT_ID, inflationDest);
    }

    account.signers.clear();

    if (withSig)
    {
        string pubKey;
        Signer signer;

        statement st = (session.prepare <<
            "SELECT publicKey, weight from Signers where accountID =:id",
            use(base58ID), into(pubKey), into(signer.weight));
        {
            auto timer = db.getSelectTimer("signer");
            st.execute(true);
        }
        while(st.got_data())
        {
            signer.pubKey = fromBase58Check256(VER_ACCOUNT_ID, pubKey);

            account.signers.push_back(signer);

            st.fetch();
        }
    }
    retAcc.mKeyCalculated = false;
    return true;
}

bool AccountFrame::exists(Database& db, LedgerKey const& key)
{
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, key.account().accountID);
    int exists = 0;
    {
        auto timer = db.getSelectTimer("account-exists");
        db.getSession() <<
            "SELECT EXISTS (SELECT NULL FROM Accounts \
             WHERE accountID=:v1)",
            use(base58ID),
            into(exists);
    }
    return exists != 0;
}

void AccountFrame::storeDelete(LedgerDelta &delta, Database &db)
{
    storeDelete(delta, db, getKey());
}

void AccountFrame::storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key)
{
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, key.account().accountID);

    soci::session &session = db.getSession();
    {
        auto timer = db.getDeleteTimer("account");
        session <<
            "DELETE from Accounts where accountID= :v1", soci::use(base58ID);
    }
    {
        auto timer = db.getDeleteTimer("signer");
        session <<
            "DELETE from Signers where accountID= :v1", soci::use(base58ID);
    }
    delta.deleteEntry(key);
}

void AccountFrame::storeUpdate(LedgerDelta &delta, Database &db, bool insert)
{
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, mAccountEntry.accountID);

    std::stringstream sql;

    if (insert)
    {
        sql << "INSERT INTO Accounts ( accountID, balance, seqNum, \
            numSubEntries, inflationDest, thresholds, flags) \
            VALUES ( :id, :v1, :v2, :v3, :v4, :v5, :v6 )";
    }
    else
    {
        sql << "UPDATE Accounts SET balance = :v1, seqNum = :v2, numSubEntries = :v3, \
                inflationDest = :v4, thresholds = :v5, \
                flags = :v6 WHERE accountID = :id";
    }

    soci::indicator inflation_ind = soci::i_null;
    string inflationDestStr;

    if(mAccountEntry.inflationDest)
    {
        inflationDestStr = toBase58Check(VER_ACCOUNT_ID, *mAccountEntry.inflationDest);
        inflation_ind = soci::i_ok;
    }

    // TODO.3   KeyValue data

    string thresholds(binToHex(mAccountEntry.thresholds));

    {
        soci::statement st = (db.getSession().prepare <<
            sql.str(), use(base58ID, "id"),
            use(mAccountEntry.balance, "v1"),
            use(mAccountEntry.seqNum, "v2"),
            use(mAccountEntry.numSubEntries, "v3"),
            use(inflationDestStr, inflation_ind, "v4"),
            use(thresholds, "v5"), use(mAccountEntry.flags, "v6"));
        {
            auto timer = insert ? db.getInsertTimer("account") : db.getUpdateTimer("account");
            st.execute(true);
        }


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
        // instead separate signatures from account, just like offers are separate entities
        AccountFrame startAccountFrame;
        // TODO: don't do this (should move the logic out, just like trustlines)
        if (!loadAccount(getID(), startAccountFrame, db, true))
        {
            throw runtime_error("could not load account!");
        }
        AccountEntry &startAccount = startAccountFrame.mAccountEntry;

        // deal with changes to Signers
        if (mAccountEntry.signers.size() < startAccount.signers.size())
        { // some signers were removed
            for (auto startSigner : startAccount.signers)
            {
                bool found = false;
                for (auto finalSigner : mAccountEntry.signers)
                {
                    if (finalSigner.pubKey == startSigner.pubKey)
                    {
                        if (finalSigner.weight != startSigner.weight)
                        {
                            std::string b58signKey = toBase58Check(VER_ACCOUNT_ID, finalSigner.pubKey);
                            {
                                auto timer = db.getUpdateTimer("signer");
                                db.getSession() << "UPDATE Signers set weight=:v1 where accountID=:v2 and publicKey=:v3",
                                    use(finalSigner.weight), use(base58ID), use(b58signKey);
                            }
                        }
                        found = true;
                        break;
                    }
                }
                if (!found)
                { // delete signer
                    std::string b58signKey = toBase58Check(VER_ACCOUNT_ID, startSigner.pubKey);

                    soci::statement st = (db.getSession().prepare <<
                        "DELETE from Signers where accountID=:v2 and publicKey=:v3",
                        use(base58ID), use(b58signKey));

                    {
                        auto timer = db.getDeleteTimer("signer");
                        st.execute(true);
                    }

                    if (st.get_affected_rows() != 1)
                    {
                        throw std::runtime_error("Could not update data in SQL");
                    }
                }
            }
        }
        else
        { // signers added or the same
            for (auto finalSigner : mAccountEntry.signers)
            {
                bool found = false;
                for (auto startSigner : startAccount.signers)
                {
                    if (finalSigner.pubKey == startSigner.pubKey)
                    {
                        if (finalSigner.weight != startSigner.weight)
                        {
                            std::string b58signKey = toBase58Check(VER_ACCOUNT_ID, finalSigner.pubKey);

                            soci::statement st = (db.getSession().prepare <<
                                "UPDATE Signers set weight=:v1 where accountID=:v2 and publicKey=:v3",
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

                    soci::statement st = (db.getSession().prepare <<
                        "INSERT INTO Signers (accountID,publicKey,weight) values (:v1,:v2,:v3)",
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

void AccountFrame::storeChange(LedgerDelta &delta, Database &db)
{
    storeUpdate(delta, db, false);
}

void AccountFrame::storeAdd(LedgerDelta &delta, Database &db)
{
    EntryFrame::pointer emptyAccount = make_shared<AccountFrame>(mAccountEntry.accountID);
    storeUpdate(delta, db, true);
}

void AccountFrame::dropAll(Database &db)
{
    db.getSession() << "DROP TABLE IF EXISTS Accounts;";
    db.getSession() << "DROP TABLE IF EXISTS Signers;";

    db.getSession() << kSQLCreateStatement1;
    db.getSession() << kSQLCreateStatement2;
}
}

