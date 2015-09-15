// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifndef _WIN32
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#endif

#include "main/srv.h"
#include "util/Logging.h"
#include <string>
#include <vector>
#include <random>
#include <cassert>

namespace stellar
{

std::string
SrvRecord::hostAndPort() const
{
    return mHostname + ":" + std::to_string(mPort);
}

std::vector<SrvRecord>
getSrvRecords(std::string const& qname)
{
    std::vector<SrvRecord> records;

#ifndef _WIN32

    LOG(INFO) << "Querying DNS for SRV record of " << qname;

    unsigned char buf[1024];
    res_init();
    int len = res_search(qname.c_str(), ns_c_in, ns_t_srv,
                         buf, sizeof(buf));
    if (len < 0)
    {
        throw std::runtime_error("DNS SRV query failed");
    }

    ns_msg msg;
    ns_initparse(buf, len, &msg);

    for (size_t i = 0; i < ns_msg_count(msg, ns_s_an); ++i)
    {
        ns_rr rr;
        ns_parserr(&msg, ns_s_an, i, &rr);
        if (ns_rr_type(rr) == ns_t_srv)
        {
            u_int16_t ttl = ns_rr_ttl(rr);
            u_int16_t const *data = reinterpret_cast<u_int16_t const*>(ns_rr_rdata(rr));
            assert(ns_rr_rdata(rr) + 6 <= ns_msg_end(msg));
            u_int16_t priority = ntohs(*data++);
            u_int16_t weight = ntohs(*data++);
            u_int16_t port = ntohs(*data++);
            char name[1024];
            if (dn_expand(ns_msg_base(msg), ns_msg_end(msg),
                          reinterpret_cast<const unsigned char*>(data),
                          name, sizeof(name)) < 0)
            {
                throw std::runtime_error("Error expanding name in SRV record");
            }
            LOG(INFO) << "DNS SRV record: " << std::string(name) << ":" << port
                      << " (prio " << priority << ", weight " << weight << ")";
            records.emplace_back(SrvRecord{ttl, priority, weight, port,
                                           std::string(name)});
        }
    }

#endif
    return records;
}

SrvRecord
pickSrvRecordByPriorityAndWeight(std::vector<SrvRecord> const& records)
{

    std::uint16_t lowestPrio = 0xffff;
    for (auto const& r : records)
    {
        lowestPrio = std::min(lowestPrio, r.mPriority);
    }

    std::vector<SrvRecord> lowPrioRecords;
    std::vector<std::uint16_t> weights;

    for (auto const& r : records)
    {
        if (r.mPriority == lowestPrio)
        {
            lowPrioRecords.push_back(r);
            weights.push_back(r.mWeight);
        }
    }

    if (lowPrioRecords.empty())
    {
        return SrvRecord();
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::discrete_distribution<> d(weights.begin(), weights.end());
    return lowPrioRecords.at(d(gen));
}


}
