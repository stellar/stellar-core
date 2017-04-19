// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SignerQueries.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"

namespace stellar
{

namespace
{

const auto DROP_SIGNERS_TABLE = "DROP TABLE IF EXISTS signers;";

const auto CREAE_SIGNERS_TABLE = "CREATE TABLE signers"
                                 "("
                                 "accountid       VARCHAR(56) NOT NULL,"
                                 "publickey       VARCHAR(56) NOT NULL,"
                                 "weight          INT         NOT NULL,"
                                 "PRIMARY KEY (accountid, publickey)"
                                 ");";

const auto CREATE_SIGNERS_INDEX =
    "CREATE INDEX signersaccount ON signers (accountid)";
}

void
createSignersTable(Database& db)
{
    db.getSession() << DROP_SIGNERS_TABLE;
    db.getSession() << CREAE_SIGNERS_TABLE;
    db.getSession() << CREATE_SIGNERS_INDEX;
}

xdr::xvector<Signer, 20u>
loadSortedSigners(Database& db, std::string const& actIDStrKey)
{
    auto result = xdr::xvector<Signer, 20u>{};
    auto pubKey = std::string{};
    auto signer = Signer{};

    auto prep = db.getPreparedStatement("SELECT publickey, weight FROM "
                                        "signers WHERE accountid =:id");
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::into(pubKey));
    st.exchange(soci::into(signer.weight));
    st.define_and_bind();
    {
        auto timer = db.getSelectTimer("signer");
        st.execute(true);
    }
    while (st.got_data())
    {
        signer.key = KeyUtils::fromStrKey<SignerKey>(pubKey);
        result.push_back(signer);
        st.fetch();
    }

    std::sort(result.begin(), result.end(),
              [](Signer const& x, Signer const& y) {
                  using xdr::operator<;
                  return x.key < y.key;
              });

    return result;
}

void
insertSigners(std::string actIDStrKey, xdr::xvector<Signer, 20> const& signers,
              Database& db)
{
    for (auto const& s : signers)
    {
        auto signerStrKey = KeyUtils::toStrKey(s.key);
        auto prep2 = db.getPreparedStatement("INSERT INTO signers "
                                             "(accountid,publickey,weight) "
                                             "VALUES (:v1,:v2,:v3)");
        auto& st = prep2.statement();
        st.exchange(soci::use(actIDStrKey));
        st.exchange(soci::use(signerStrKey));
        st.exchange(soci::use(s.weight));
        st.define_and_bind();
        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
}

void
updateSigners(std::string actIDStrKey,
              xdr::xvector<Signer, 20> const& newSigners, Database& db)
{
    // generates a diff with the signers stored in the database

    // first, load the signers stored in the database for this account
    auto signers = loadSortedSigners(db, actIDStrKey);

    auto it_new = newSigners.begin();
    auto it_old = signers.begin();
    // iterate over both sets from smallest to biggest key
    while (it_new != newSigners.end() || it_old != signers.end())
    {
        bool updated = false, added = false;

        if (it_old == signers.end())
        {
            added = true;
        }
        else if (it_new != newSigners.end())
        {
            using xdr::operator==;
            updated = (it_new->key == it_old->key);
            if (!updated)
            {
                using xdr::operator<;
                added = (it_new->key < it_old->key);
            }
        }
        else
        {
            // deleted
        }

        if (updated)
        {
            if (it_new->weight != it_old->weight)
            {
                std::string signerStrKey = KeyUtils::toStrKey(it_new->key);
                auto timer = db.getUpdateTimer("signer");
                auto prep2 = db.getPreparedStatement(
                    "UPDATE signers set weight=:v1 WHERE "
                    "accountid=:v2 AND publickey=:v3");
                auto& st = prep2.statement();
                st.exchange(soci::use(it_new->weight));
                st.exchange(soci::use(actIDStrKey));
                st.exchange(soci::use(signerStrKey));
                st.define_and_bind();
                st.execute(true);
                if (st.get_affected_rows() != 1)
                {
                    throw std::runtime_error("Could not update data in SQL");
                }
            }
            it_new++;
            it_old++;
        }
        else if (added)
        {
            // signer was added
            std::string signerStrKey = KeyUtils::toStrKey(it_new->key);

            auto prep2 = db.getPreparedStatement("INSERT INTO signers "
                                                 "(accountid,publickey,weight) "
                                                 "VALUES (:v1,:v2,:v3)");
            auto& st = prep2.statement();
            st.exchange(soci::use(actIDStrKey));
            st.exchange(soci::use(signerStrKey));
            st.exchange(soci::use(it_new->weight));
            st.define_and_bind();
            st.execute(true);

            if (st.get_affected_rows() != 1)
            {
                throw std::runtime_error("Could not update data in SQL");
            }
            it_new++;
        }
        else
        {
            // signer was deleted
            std::string signerStrKey = KeyUtils::toStrKey(it_old->key);

            auto prep2 = db.getPreparedStatement("DELETE from signers WHERE "
                                                 "accountid=:v2 AND "
                                                 "publickey=:v3");
            auto& st = prep2.statement();
            st.exchange(soci::use(actIDStrKey));
            st.exchange(soci::use(signerStrKey));
            st.define_and_bind();
            {
                auto timer = db.getDeleteTimer("signer");
                st.execute(true);
            }

            if (st.get_affected_rows() != 1)
            {
                throw std::runtime_error("Could not update data in SQL");
            }

            it_old++;
        }
    }
}

optional<AccountID>
selectSignerWithoutAccount(Database& db)
{
    auto prep = db.getPreparedStatement(
        "SELECT signers.accountid FROM signers "
        "LEFT OUTER JOIN accounts ON signers.accountid = accounts.accountid "
        "WHERE accounts.accountid IS NULL "
        "LIMIT 1");
    auto& st = prep.statement();
    auto pubKey = std::string{};

    st.exchange(soci::into(pubKey));
    st.define_and_bind();
    st.execute(true);

    if (st.got_data())
    {
        return make_optional<AccountID>(
            KeyUtils::fromStrKey<PublicKey>(pubKey));
    }

    return nullopt<AccountID>();
}
}
