// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/BanManagerImpl.h"
#include "crypto/KeyUtils.h"
#include "database/Database.h"
#include "main/Application.h"
#include "util/Logging.h"
#include <Tracy.hpp>

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
    ZoneScoped;
    if (isBanned(nodeID))
    {
        return;
    }

    auto nodeIDString = KeyUtils::toStrKey(nodeID);

    CLOG_INFO(Overlay, "ban {}", nodeIDString);

    {
        ZoneNamedN(insertBanZone, "insert ban", true);
        auto prep = mApp.getDatabase().getPreparedStatement(
            "INSERT INTO ban (nodeid) VALUES(:n)",
            mApp.getDatabase().getSession());
        auto& st = prep.statement();
        st.exchange(soci::use(nodeIDString));
        st.define_and_bind();
        st.execute(true);
    }
}

void
BanManagerImpl::unbanNode(NodeID nodeID)
{
    ZoneScoped;
    auto nodeIDString = KeyUtils::toStrKey(nodeID);
    CLOG_INFO(Overlay, "unban {}", nodeIDString);
    {
        ZoneNamedN(deleteBanZone, "delete ban", true);
        auto prep = mApp.getDatabase().getPreparedStatement(
            "DELETE FROM ban WHERE nodeid = :n;",
            mApp.getDatabase().getSession());
        auto& st = prep.statement();
        st.exchange(soci::use(nodeIDString));
        st.define_and_bind();
        st.execute(true);
    }
}

bool
BanManagerImpl::isBanned(NodeID nodeID)
{
    ZoneScoped;
    auto nodeIDString = KeyUtils::toStrKey(nodeID);
    {
        ZoneNamedN(selectBanZone, "select ban", true);
        auto prep = mApp.getDatabase().getPreparedStatement(
            "SELECT count(*) FROM ban WHERE nodeid = :n",
            mApp.getDatabase().getSession());
        uint32_t count;
        auto& st = prep.statement();
        st.exchange(soci::into(count));
        st.exchange(soci::use(nodeIDString));
        st.define_and_bind();
        st.execute(true);
        return count == 1;
    }
}

std::vector<std::string>
BanManagerImpl::getBans()
{
    ZoneScoped;
    std::vector<std::string> result;
    std::string nodeIDString;
    {
        ZoneNamedN(selectBanZone, "select ban", true);
        auto prep = mApp.getDatabase().getPreparedStatement(
            "SELECT nodeid FROM ban", mApp.getDatabase().getSession());
        auto& st = prep.statement();
        st.exchange(soci::into(nodeIDString));
        st.define_and_bind();
        st.execute(true);
        while (st.got_data())
        {
            result.push_back(nodeIDString);
            st.fetch();
        }
    }
    return result;
}

void
BanManager::dropAll(Database& db)
{
    db.getRawSession() << "DROP TABLE IF EXISTS ban";

    db.getRawSession() << "CREATE TABLE ban ("
                          "nodeid      CHARACTER(56) NOT NULL PRIMARY KEY"
                          ")";
}
}
