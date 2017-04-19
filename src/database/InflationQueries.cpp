// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "InflationQueries.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"

namespace stellar
{

std::vector<InflationWinner>
inflationWinners(int64_t minVotes, int maxWinners, Database& db)
{
    auto result = std::vector<InflationWinner>{};
    auto& session = db.getSession();

    InflationWinner v;
    auto inflationDest = std::string{};

    soci::statement st =
        (session.prepare
             << "SELECT"
                " SUM(balance) AS votes, inflationdest FROM accounts"
                " WHERE inflationdest IS NOT NULL"
                " AND balance >= 1000000000 GROUP BY inflationdest"
                " ORDER BY votes DESC, inflationdest DESC LIMIT :lim",
         soci::into(v.mVotes), soci::into(inflationDest),
         soci::use(maxWinners));

    st.execute(true);

    while (st.got_data())
    {
        if (v.mVotes < minVotes)
        {
            break;
        }

        v.mInflationDest = KeyUtils::fromStrKey<PublicKey>(inflationDest);
        result.push_back(v);
        st.fetch();
    }

    return result;
}
}
