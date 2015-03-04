// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "AccountFrame.h"
#include "lib/json/json.h"
#include "crypto/Base58.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "LedgerDelta.h"

using namespace soci;
using namespace std;


namespace stellar
{
const char *AccountFrame::kSQLCreateStatement1 =
    "CREATE TABLE Accounts"
     "("
     "accountID       VARCHAR(51)  PRIMARY KEY,"
     "balance         BIGINT       NOT NULL,"
     "numSubEntries   INT          NOT NULL DEFAULT 0 CHECK (numSubEntries >= 0),"
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

const char *AccountFrame::kSQLCreateStatement3 =
    "CREATE TABLE AccountData"
     "("
     "accountID       VARCHAR(51)    PRIMARY KEY,"
     "key             INT            NOT NULL,"
     "value           TEXT           NOT NULL"
     ");";

const char *AccountFrame::kSQLCreateStatement4 =
    "CREATE TABLE SeqSlots"
    "("
    "accountID       VARCHAR(51) NOT NULL,"
    "seqSlot         INT         NOT NULL,"
    "seqNum          INT         NOT NULL,"
    "PRIMARY KEY (accountID, seqSlot)"
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

xdr::xvector<Signer> &AccountFrame::getSigners()
{
    return mAccountEntry.signers;
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
        session << "SELECT balance,numSubEntries, \
            inflationDest, thresholds,  flags from Accounts where accountID=:v1",
            into(account.balance), into(account.numSubEntries),
            into(inflationDest, inflationDestInd),
            into(thresholds, thresholdsInd), into(account.flags),
            use(base58ID);
    }


    if (!session.got_data())
        return false;

    if (thresholdsInd == soci::i_ok)
    {
        std::vector<uint8_t> bin = hexToBin(thresholds);
        for (int n = 0; (n < 4) && (n < bin.size()); n++)
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

uint32_t AccountFrame::getSeq(uint32_t slot,Database& db)
{
    auto i = mUpdatedSeqNums.find(slot);
    if(i == mUpdatedSeqNums.end())
    { // seq num not changed
        std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getID());

        soci::session &session = db.getSession();
        uint32_t retNum = 0;

        session << "SELECT seqNum from SeqSlots where accountID=:v1 and seqSlot=:v2",
            into(retNum),use(base58ID),use(slot);

        return retNum;
    }
    
    return mUpdatedSeqNums[slot];
}

uint32_t AccountFrame::getMaxSeqSlot(Database& db)
{
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getID());

    soci::session &session = db.getSession();
    uint32_t retNum = 0;

    session << "SELECT max(seqSlot) from SeqSlots where accountID=:v1",
        into(retNum);

    return retNum;
}

void AccountFrame::setSeqSlot(uint32_t slot, uint32_t seq)
{
    mUpdatedSeqNums[slot] = seq;
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
        auto timer = db.getDeleteTimer("account-data");
        session <<
            "DELETE from AccountData where accountID= :v1", soci::use(base58ID);
    }
    {
        auto timer = db.getDeleteTimer("signer");
        session <<
            "DELETE from Signers where accountID= :v1", soci::use(base58ID);
    }
    {
        auto timer = db.getDeleteTimer("slot");
        session <<
            "DELETE from SeqSlots where accountID= :v1", soci::use(base58ID);
    }
    delta.deleteEntry(key);
}

void AccountFrame::storeUpdate(LedgerDelta &delta, Database &db, bool insert)
{
    AccountEntry& finalAccount = mAccountEntry;
    std::string base58ID = toBase58Check(VER_ACCOUNT_ID, finalAccount.accountID);

    std::stringstream sql;

    if (insert)
    {
        sql << "INSERT INTO Accounts ( accountID, balance,   \
            numSubEntries, inflationDest, thresholds, flags) \
            VALUES ( :id, :v1, :v2, :v3, :v4, :v5 )";

        {
            auto timer = db.getInsertTimer("slot");
            db.getSession() << "INSERT into SeqSlots (accountID,seqSlot,seqNum) values (:v1,0,0)",
                use(base58ID);
        }

    }
    else
    {
        sql << "UPDATE Accounts SET balance = :v1, numSubEntries = :v2, \
                inflationDest = :v3, thresholds = :v4, \
                flags = :v5 WHERE accountID = :id";
    }

    soci::indicator inflation_ind = soci::i_null;
    string inflationDestStr;

    if(finalAccount.inflationDest)
    {
        inflationDestStr = toBase58Check(VER_ACCOUNT_ID, *finalAccount.inflationDest);
        inflation_ind = soci::i_ok;
    }

    // TODO.3   KeyValue data

    string thresholds(binToHex(finalAccount.thresholds));

    {
        soci::statement st = (db.getSession().prepare <<
            sql.str(), use(base58ID, "id"),
            use(finalAccount.balance, "v1"), 
            use(finalAccount.numSubEntries, "v2"),
            use(inflationDestStr, inflation_ind, "v3"),
            use(thresholds, "v4"), use(finalAccount.flags, "v5"));
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

    if(mUpdatedSeqNums.size())
    {
        for(auto slot : mUpdatedSeqNums)
        {
            {
                auto timer = db.getUpdateTimer("slot");
                db.getSession() << "UPDATE SeqSlots set seqNum=:v1 where accountID=:v2 and seqSlot=:v3",
                    use(slot.second), use(base58ID), use(slot.first);
            }
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
    db.getSession() << "DROP TABLE IF EXISTS AccountData;";
    db.getSession() << "DROP TABLE IF EXISTS SeqSlots;";

    db.getSession() << kSQLCreateStatement1;
    db.getSession() << kSQLCreateStatement2;
    db.getSession() << kSQLCreateStatement3;
    db.getSession() << kSQLCreateStatement4;
}
}

