#include "main/Diagnostics.h"
#include "json/json.h"
#include "util/Decoder.h"
#include "util/XDROperators.h"
#include "util/XDRStream.h"

#include <map>
#include <numeric>

namespace stellar
{
namespace diagnostics
{

void
bucketStats(std::string const& filename, bool aggregateAccounts)
{
    XDRInputFileStream in;
    in.open(filename);

    BucketEntry tmp;
    std::map<LedgerEntryType, size_t> ledgerEntriesSizeBytes,
        ledgerEntriesCount, deadEntryCount;
    std::map<BucketEntryType, size_t> bucketEntriesCount;
    std::map<AccountID, size_t> perAccountBytes, perAccountCount;
    Json::Value root;

    while (in && in.readOne(tmp))
    {
        ++bucketEntriesCount[tmp.type()];
        switch (tmp.type())
        {
        case LIVEENTRY:
        case INITENTRY:
        {
            auto const& le = tmp.liveEntry().data;
            auto t = le.type();
            auto bytes = xdr::xdr_to_opaque(tmp).size();
            ledgerEntriesSizeBytes[t] += bytes;
            ++ledgerEntriesCount[t];

            if (aggregateAccounts)
            {
                AccountID const* k = nullptr;
                switch (t)
                {
                case ACCOUNT:
                    k = &le.account().accountID;
                    break;
                case TRUSTLINE:
                    k = &le.trustLine().accountID;
                    break;
                case OFFER:
                    k = &le.offer().sellerID;
                    break;
                case DATA:
                    k = &le.data().accountID;
                    break;
                default:
                    break;
                }
                if (k)
                {
                    perAccountBytes[*k] += bytes;
                    ++perAccountCount[*k];
                }
            }
        }
        break;
        case DEADENTRY:
            ++deadEntryCount[tmp.deadEntry().type()];
            break;
        case METAENTRY:
            root["LEDGER_VERSION"] = tmp.metaEntry().ledgerVersion;
        }
    }
    Json::Value& be = root["BUCKET_ENTRIES"];
    for (auto const& kv : bucketEntriesCount)
    {
        be[xdr::xdr_traits<BucketEntryType>::enum_name(kv.first)] =
            static_cast<Json::Int64>(kv.second);
    }
    Json::Value& le = root["LEDGER_ENTRIES"];
    for (auto const& kv : ledgerEntriesCount)
    {
        auto t = kv.first;
        auto n = kv.second;
        auto totalBytes = ledgerEntriesSizeBytes[t];
        auto avgBytes = totalBytes / n;
        size_t deadCount = 0;
        auto deadIt = deadEntryCount.find(t);
        if (deadIt != deadEntryCount.end())
        {
            deadCount = deadIt->second;
        }
        Json::Value& o =
            le[xdr::xdr_traits<LedgerEntryType>::enum_name(kv.first)];
        o["count"] = static_cast<Json::Int64>(n);
        o["dead_count"] = static_cast<Json::Int64>(deadCount);
        o["avg"] = static_cast<Json::Int64>(avgBytes);
    }
    if (aggregateAccounts)
    {
        // compute per account stats
        std::vector<size_t> bytes, entries;
        bytes.reserve(perAccountBytes.size());
        entries.reserve(perAccountBytes.size());

        size_t totalBytes = 0;
        size_t totalEntries = 0;

        for (auto const& kv : perAccountCount)
        {
            auto n = kv.second;
            auto kvTotalBytes = perAccountBytes[kv.first];

            bytes.emplace_back(kvTotalBytes);
            totalBytes += kvTotalBytes;

            entries.emplace_back(n);
            totalEntries += n;
        }
        std::sort(bytes.begin(), bytes.end());
        std::sort(entries.begin(), entries.end());

        auto addStats = [&](Json::Value& o, std::vector<size_t> const& v,
                            size_t total) {
            auto vMin = v[0];
            auto vMax = v[v.size() - 1];
            o["count"] = static_cast<Json::Int64>(v.size());
            o["total"] = static_cast<Json::Int64>(total);
            o["min"] = static_cast<Json::Int64>(vMin);
            o["max"] = static_cast<Json::Int64>(vMax);
            o["avg"] = static_cast<Json::Int64>(total / v.size());
            o["P50"] = static_cast<Json::Int64>(v[v.size() / 2]);
            o["P99"] = static_cast<Json::Int64>(v[v.size() * 99 / 100]);
        };

        Json::Value& agg = root["AGG_ACCOUNT"];
        addStats(agg["BYTES"], bytes, totalBytes);
        addStats(agg["ENTRIES"], entries, totalEntries);
    }

    fmt::print("{}\n", root.toStyledString());
}
}
}
