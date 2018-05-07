// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/BanManagerImpl.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

using namespace std;

std::unique_ptr<BanManager>
BanManager::create(Application& app)
{
    return std::make_unique<BanManagerImpl>(app);
}

BanManagerImpl::BanManagerImpl(Application& app) : mApp(app)
{
}

BanManagerImpl::~BanManagerImpl()
{
}

void
BanManagerImpl::banNode(NodeID nodeID)
{
    auto nodeIDString = KeyUtils::toStrKey(nodeID);
    auto timer = mApp.getDatabase().getInsertTimer("ban");
    auto prep = mApp.getDatabase().getPreparedStatement(
        "INSERT INTO ban (nodeid) "
        "SELECT :n WHERE NOT EXISTS (SELECT 1 FROM ban WHERE nodeid = :n)");
    auto& st = prep.statement();
    st.exchange(soci::use(nodeIDString));
    st.define_and_bind();
    st.execute(true);
}

void
BanManagerImpl::unbanNode(NodeID nodeID)
{
    auto nodeIDString = KeyUtils::toStrKey(nodeID);
    auto timer = mApp.getDatabase().getDeleteTimer("ban");
    auto prep = mApp.getDatabase().getPreparedStatement(
        "DELETE FROM ban WHERE nodeid = :n;");
    auto& st = prep.statement();
    st.exchange(soci::use(nodeIDString));
    st.define_and_bind();
    st.execute(true);
}

bool
BanManagerImpl::isBanned(NodeID nodeID)
{
    auto nodeIDString = KeyUtils::toStrKey(nodeID);
    auto timer = mApp.getDatabase().getSelectTimer("ban");
    auto prep = mApp.getDatabase().getPreparedStatement(
        "SELECT count(*) FROM ban WHERE nodeid = :n");
    uint32_t count;
    auto& st = prep.statement();
    st.exchange(soci::into(count));
    st.exchange(soci::use(nodeIDString));
    st.define_and_bind();
    st.execute(true);
    return count == 1;
}

std::vector<std::string>
BanManagerImpl::getBans()
{
    std::vector<std::string> result;
    std::string nodeIDString;
    auto timer = mApp.getDatabase().getSelectTimer("ban");
    auto prep =
        mApp.getDatabase().getPreparedStatement("SELECT nodeid FROM ban");
    auto& st = prep.statement();
    st.exchange(soci::into(nodeIDString));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
        result.push_back(nodeIDString);
        st.fetch();
    }
    return result;
}

void
BanManager::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS ban";

    db.getSession() << "CREATE TABLE ban ("
                       "nodeid      CHARACTER(56) NOT NULL PRIMARY KEY"
                       ")";
}
}
