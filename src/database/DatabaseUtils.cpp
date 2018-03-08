// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "DatabaseUtils.h"

namespace stellar
{
namespace DatabaseUtils
{
void
deleteOldEntriesHelper(soci::session& sess, uint32_t ledgerSeq, uint32_t count,
                       std::string const& tableName,
                       std::string const& ledgerSeqColumn)
{
    uint32_t curMin = 0;
    soci::indicator gotMin;
    soci::statement st = (sess.prepare << "SELECT MIN(" << ledgerSeqColumn
                                       << ") FROM " << tableName,
                          soci::into(curMin, gotMin));

    st.execute(true);
    if (st.got_data() && gotMin == soci::i_ok)
    {
        uint64 m64 =
            std::min<uint64>(static_cast<uint64>(curMin) + count, ledgerSeq);
        // safe to cast down as it's at most ledgerSeq
        uint64 m = static_cast<uint32>(m64);
        sess << "DELETE FROM " << tableName << " WHERE " << ledgerSeqColumn
             << " <= " << m;
    }
}
}
}