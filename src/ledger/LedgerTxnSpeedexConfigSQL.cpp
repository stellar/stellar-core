// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxnImpl.h"
#include "util/GlobalChecks.h"

namespace stellar
{
namespace
{

const std::string CREATE_SPEEDEX_CONFIGURATION_TABLE_SQL = R"(
    CREATE TABLE speedexconfiguration (
        dummykey     INT PRIMARY KEY,
        ledgerentry  TEXT NOT NULL,
        lastmodified INT NOT NULL);
)";

const std::string DROP_SPEEDEX_CONFIGURATION_TABLE_SQL =
    "DROP TABLE IF EXISTS speedexconfiguration;";

const std::string LOAD_SPEEDEX_CONFIGURATION_SQL = R"(
    SELECT ledgerentry
    FROM speedexconfiguration
    LIMIT 1;)";

const std::string INSERT_SPEEDEX_CONFIGURATION_SQL = R"(
    INSERT INTO speedexconfiguration
    VALUES ( 
        1, :ledgerentry, :lastmodified 
    )
    ON CONFLICT (dummykey) DO UPDATE SET
        ledgerentry = excluded.ledgerentry,
        lastmodified = excluded.lastmodified;
)";

}

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadSpeedexConfiguration() const
{
    std::string speedexConfigStr;

    auto prep = mDatabase.getPreparedStatement(LOAD_SPEEDEX_CONFIGURATION_SQL);
    auto& st = prep.statement();
    st.exchange(soci::into(speedexConfigStr));
    st.define_and_bind();
    st.execute(true);
    if (!st.got_data())
    {
        return nullptr;
    }

    LedgerEntry speedexConfigEntry;
    fromOpaqueBase64(speedexConfigEntry, speedexConfigStr);
    releaseAssert(speedexConfigEntry.data.type() == SPEEDEX_CONFIGURATION);

    return std::make_shared<LedgerEntry const>(std::move(speedexConfigEntry));
}

void
LedgerTxnRoot::Impl::upsertSpeedexConfiguration(EntryIterator const& entry)
{
    auto prep =
        mDatabase.getPreparedStatement(INSERT_SPEEDEX_CONFIGURATION_SQL);
    auto& st = prep.statement();
    auto const& ledgerEntry = entry.entry().ledgerEntry();
    auto ledgerEntryStr = toOpaqueBase64(ledgerEntry);
    st.exchange(soci::use(ledgerEntryStr));
    st.exchange(soci::use(ledgerEntry.lastModifiedLedgerSeq));
    st.define_and_bind();
    {
        auto timer = mDatabase.getUpsertTimer("speedexconfiguration");
        st.execute(true);
    }
    if (static_cast<size_t>(st.get_affected_rows()) != 1)
    {
        throw std::runtime_error(
            "Could not update speedex configuration data in SQL");
    }
}

void
LedgerTxnRoot::Impl::dropSpeedexConfiguration()
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffers.clear();

    mDatabase.getSession() << DROP_SPEEDEX_CONFIGURATION_TABLE_SQL;
    mDatabase.getSession() << CREATE_SPEEDEX_CONFIGURATION_TABLE_SQL;
}
}
