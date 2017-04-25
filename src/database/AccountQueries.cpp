// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "AccountQueries.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"

namespace stellar
{

uint64_t
sumOfBalances(Database& db)
{
    auto sum = uint64_t{0};
    auto prep = db.getPreparedStatement("SELECT SUM(balance) FROM accounts;");

    auto& st = prep.statement();
    st.exchange(soci::into(sum));
    st.define_and_bind();
    st.execute(true);

    return sum;
}

NumberOfSubentries
numberOfSubentries(AccountID const& accountID, Database& db)
{
    auto result = NumberOfSubentries{};
    auto actIDStrKey = KeyUtils::toStrKey(accountID);

    auto query = std::string{R"(
        SELECT numsubentries,
              (SELECT COUNT(*) FROM trustlines WHERE accountid = :id)
            + (SELECT COUNT(*) FROM offers WHERE sellerid = :id)
            + (SELECT COUNT(*) FROM accountdata WHERE accountid = :id)
            + (SELECT COUNT(*) FROM signers WHERE accountid = :id)
        FROM accounts
        WHERE accountid = :id
    )"};

    auto prep = db.getPreparedStatement(query);
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey, "id"));
    st.exchange(soci::into(result.inAccountsTable));
    st.exchange(soci::into(result.calculated));
    st.define_and_bind();
    st.execute(true);

    return result;
}
}
