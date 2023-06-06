
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "herder/Herder.h"
#include "history/HistoryArchive.h"
#include "ledger/LedgerManager.h"
#include "main/ExternalQueue.h"
#include "main/StellarCoreVersion.h"
#include "scp/LocalNode.h"
#include "scp/QuorumSetUtils.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"

#include "overlay/OverlayManager.h"
#include "util/UnorderedSet.h"
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <functional>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

namespace stellar
{
const uint32 Config::CURRENT_LEDGER_PROTOCOL_VERSION = 19
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
                                                       + 1
#endif
    ;

bool gIsProductionNetwork = false;

// Options that must only be used for testing
static const std::unordered_set<std::string> TESTING_ONLY_OPTIONS = {
    "RUN_STANDALONE",
    "MANUAL_CLOSE",
    "ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING",
    "ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING",
    "ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING",
    "ARTIFICIALLY_REPLAY_WITH_NEWEST_BUCKET_LOGIC_FOR_TESTING",
    "OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING",
    "OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING",
    "LOADGEN_OP_COUNT_FOR_TESTING",
    "LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING",
    "CATCHUP_WAIT_MERGES_TX_APPLY_FOR_TESTING",
    "ARTIFICIALLY_DELAY_BUCKET_APPLICATION_FOR_TESTING",
    "ARTIFICIALLY_SLEEP_MAIN_THREAD_FOR_TESTING",
    "ARTIFICIALLY_SKIP_CONNECTION_ADJUSTMENT_FOR_TESTING"};

// Options that should only be used for testing
static const std::unordered_set<std::string> TESTING_SUGGESTED_OPTIONS = {
    "ALLOW_LOCALHOST_FOR_TESTING"};

namespace
{
// compute a default threshold for qset:
// if thresholdLevel is SIMPLE_MAJORITY there are no inner sets, only
// require majority
// (>50%). If thresholdLevel is ALL_REQUIRED, require 100%, otherwise assume
// byzantine failures (~67%)
unsigned int
computeDefaultThreshold(SCPQuorumSet const& qset,
                        ValidationThresholdLevels thresholdLevel)
{
    unsigned int res = 0;
    unsigned int topSize = static_cast<unsigned int>(qset.validators.size() +
                                                     qset.innerSets.size());

    if (topSize == 0)
    {
        // leave the quorum set empty
        return 0;
    }

    if (thresholdLevel == ValidationThresholdLevels::SIMPLE_MAJORITY &&
        qset.innerSets.empty())
    {
        // n=2f+1
        // compute res = n - f
        res = topSize - (topSize - 1) / 2;
    }
    else if (thresholdLevel == ValidationThresholdLevels::ALL_REQUIRED)
    {
        // all are required
        res = topSize;
    }
    else
    {
        // n=3f+1
        // compute res = n - f
        res = topSize - (topSize - 1) / 3;
    }
    return res;
}
}
Config::Config() : NODE_SEED(SecretKey::random())
{
    // fill in defaults

    // non configurable
    MODE_ENABLES_BUCKETLIST = true;
    MODE_USES_IN_MEMORY_LEDGER = false;
    MODE_STORES_HISTORY_MISC = true;
    MODE_STORES_HISTORY_LEDGERHEADERS = true;
    MODE_DOES_CATCHUP = true;
    MODE_AUTO_STARTS_OVERLAY = true;
    OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING =
        std::vector<std::chrono::microseconds>();
    OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING = std::vector<uint32>();
    LOADGEN_OP_COUNT_FOR_TESTING = {};
    LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING = {};
    CATCHUP_WAIT_MERGES_TX_APPLY_FOR_TESTING = false;
    ARTIFICIALLY_SLEEP_MAIN_THREAD_FOR_TESTING =
        std::chrono::microseconds::zero();

    FORCE_SCP = false;
    LEDGER_PROTOCOL_VERSION = CURRENT_LEDGER_PROTOCOL_VERSION;
    LEDGER_PROTOCOL_MIN_VERSION_INTERNAL_ERROR_REPORT = 18;

    OVERLAY_PROTOCOL_MIN_VERSION = 27;
    OVERLAY_PROTOCOL_VERSION = 28;

    VERSION_STR = STELLAR_CORE_VERSION;

    // configurable
    RUN_STANDALONE = false;
    MANUAL_CLOSE = false;
    CATCHUP_COMPLETE = false;
    CATCHUP_RECENT = 0;
    EXPERIMENTAL_PRECAUTION_DELAY_META = false;
    EXPERIMENTAL_BUCKETLIST_DB = false;
    EXPERIMENTAL_BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT = 14; // 2^14 == 16 kb
    EXPERIMENTAL_BUCKETLIST_DB_INDEX_CUTOFF = 20;             // 20 mb
    EXPERIMENTAL_BUCKETLIST_DB_PERSIST_INDEX = true;
    // automatic maintenance settings:
    // short and prime with 1 hour which will cause automatic maintenance to
    // rarely conflict with any other scheduled tasks on a machine (that tend to
    // run on a fixed schedule)
    AUTOMATIC_MAINTENANCE_PERIOD = std::chrono::seconds{359};
    // count picked as to catchup with 1 month worth of ledgers
    // in about 1 week.
    // (30*24*3600/5) / (400 - 359/5 ) // number of periods needed to catchup
    //   * (359) / (24*3600) = 6.56 days
    AUTOMATIC_MAINTENANCE_COUNT = 400;
    // automatic self-check happens once every 3 hours
    AUTOMATIC_SELF_CHECK_PERIOD = std::chrono::seconds{3 * 60 * 60};
    ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = false;
    ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = false;
    ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING = 0;
    ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = false;
    ARTIFICIALLY_REDUCE_MERGE_COUNTS_FOR_TESTING = false;
    ARTIFICIALLY_SKIP_CONNECTION_ADJUSTMENT_FOR_TESTING = false;
    ARTIFICIALLY_REPLAY_WITH_NEWEST_BUCKET_LOGIC_FOR_TESTING = false;
    ARTIFICIALLY_DELAY_BUCKET_APPLICATION_FOR_TESTING =
        std::chrono::seconds::zero();
    ALLOW_LOCALHOST_FOR_TESTING = false;
    USE_CONFIG_FOR_GENESIS = false;
    FAILURE_SAFETY = -1;
    UNSAFE_QUORUM = false;
    LIMIT_TX_QUEUE_SOURCE_ACCOUNT = false;
    DISABLE_BUCKET_GC = false;
    DISABLE_XDR_FSYNC = false;
    MAX_SLOTS_TO_REMEMBER = 12;
    // Configure MAXIMUM_LEDGER_CLOSETIME_DRIFT based on MAX_SLOTS_TO_REMEMBER
    // (plus a small buffer) to make sure we don't reject SCP state sent to us
    // by default. Limit allowed drift to 90 seconds as to not overwhelm the
    // node too much.
    uint32_t CLOSETIME_DRIFT_LIMIT = 90;
    MAXIMUM_LEDGER_CLOSETIME_DRIFT =
        std::min<uint32_t>((MAX_SLOTS_TO_REMEMBER + 2) *
                               Herder::EXP_LEDGER_TIMESPAN_SECONDS.count(),
                           CLOSETIME_DRIFT_LIMIT);
    METADATA_OUTPUT_STREAM = "";
    METADATA_DEBUG_LEDGERS = 0;

    LOG_FILE_PATH = "stellar-core-{datetime:%Y-%m-%d_%H-%M-%S}.log";
    BUCKET_DIR_PATH = "buckets";

    LOG_COLOR = false;

    TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = LEDGER_PROTOCOL_VERSION;
    TESTING_UPGRADE_DESIRED_FEE = LedgerManager::GENESIS_LEDGER_BASE_FEE;
    TESTING_UPGRADE_RESERVE = LedgerManager::GENESIS_LEDGER_BASE_RESERVE;
    TESTING_UPGRADE_MAX_TX_SET_SIZE = 50;
    TESTING_UPGRADE_FLAGS = 0;
    TESTING_LEDGER_MAX_PROPAGATE_SIZE_BYTES =
        1 * InitialSorobanNetworkConfig::TX_MAX_SIZE_BYTES;
    TESTING_LEDGER_MAX_INSTRUCTIONS =
        1 * InitialSorobanNetworkConfig::TX_MAX_INSTRUCTIONS;
    TESTING_LEDGER_MAX_READ_LEDGER_ENTRIES =
        1 * InitialSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES;
    TESTING_LEDGER_MAX_READ_BYTES =
        1 * InitialSorobanNetworkConfig::TX_MAX_READ_BYTES;
    TESTING_LEDGER_MAX_WRITE_LEDGER_ENTRIES =
        1 * InitialSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES;
    TESTING_LEDGER_MAX_WRITE_BYTES =
        1 * InitialSorobanNetworkConfig::TX_MAX_WRITE_BYTES;

    HTTP_PORT = DEFAULT_PEER_PORT + 1;
    PUBLIC_HTTP_PORT = false;
    HTTP_MAX_CLIENT = 128;
    PEER_PORT = DEFAULT_PEER_PORT;
    TARGET_PEER_CONNECTIONS = 8;
    MAX_PENDING_CONNECTIONS = 500;
    MAX_ADDITIONAL_PEER_CONNECTIONS = -1;
    MAX_OUTBOUND_PENDING_CONNECTIONS = 0;
    MAX_INBOUND_PENDING_CONNECTIONS = 0;
    PEER_AUTHENTICATION_TIMEOUT = 2;
    PEER_TIMEOUT = 30;
    PEER_STRAGGLER_TIMEOUT = 120;

    FLOOD_OP_RATE_PER_LEDGER = 1.0;
    FLOOD_TX_PERIOD_MS = 200;
    FLOOD_ARB_TX_BASE_ALLOWANCE = 5;
    FLOOD_ARB_TX_DAMPING_FACTOR = 0.8;

    FLOOD_DEMAND_PERIOD_MS = std::chrono::milliseconds(200);
    FLOOD_ADVERT_PERIOD_MS = std::chrono::milliseconds(100);
    FLOOD_DEMAND_BACKOFF_DELAY_MS = std::chrono::milliseconds(500);

    MAX_BATCH_WRITE_COUNT = 1024;
    MAX_BATCH_WRITE_BYTES = 1 * 1024 * 1024;
    PREFERRED_PEERS_ONLY = false;

    PEER_READING_CAPACITY = 200;
    PEER_FLOOD_READING_CAPACITY = 200;
    FLOW_CONTROL_SEND_MORE_BATCH_SIZE = 40;

    PEER_FLOOD_READING_CAPACITY_BYTES = 300000;
    FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = 100000;
    OUTBOUND_TX_QUEUE_BYTE_LIMIT = 1024 * 1024 * 3;
    ENABLE_FLOW_CONTROL_BYTES = true;

    // WORKER_THREADS: setting this too low risks a form of priority inversion
    // where a long-running background task occupies all worker threads and
    // we're not able to do short high-priority background tasks like merging
    // small buckets to be ready for the next ledger close. To attempt to
    // mitigate this, we make sure we have as many worker threads as the worst
    // case long-running parallelism we're going to encounter, and let the OS
    // deal with time-slicing between the threads if there aren't enough cores
    // for it.
    //
    // Worst case = 10 concurrent merges + 1 quorum intersection calculation.
    WORKER_THREADS = 11;
    MAX_CONCURRENT_SUBPROCESSES = 16;
    NODE_IS_VALIDATOR = false;
    QUORUM_INTERSECTION_CHECKER = true;
    DATABASE = SecretValue{"sqlite3://:memory:"};

    ENTRY_CACHE_SIZE = 100000;
    PREFETCH_BATCH_SIZE = 1000;

    HISTOGRAM_WINDOW_SIZE = std::chrono::seconds(30);

    HALT_ON_INTERNAL_TRANSACTION_ERROR = false;

    MAX_DEX_TX_OPERATIONS_IN_TX_SET = std::nullopt;

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = false;
#endif

#ifdef BUILD_TESTS
    TEST_CASES_ENABLED = false;
#endif

#ifdef BEST_OFFER_DEBUGGING
    BEST_OFFER_DEBUGGING_ENABLED = false;
#endif
}

namespace
{

using ConfigItem = std::pair<std::string, std::shared_ptr<cpptoml::base>>;

bool
readBool(ConfigItem const& item)
{
    if (!item.second->as<bool>())
    {
        throw std::invalid_argument(
            fmt::format(FMT_STRING("invalid '{}'"), item.first));
    }
    return item.second->as<bool>()->get();
}

double
readDouble(ConfigItem const& item)
{
    if (!item.second->as<double>())
    {
        throw std::invalid_argument(
            fmt::format(FMT_STRING("invalid '{}'"), item.first));
    }
    return item.second->as<double>()->get();
}

std::string
readString(ConfigItem const& item)
{
    if (!item.second->as<std::string>())
    {
        throw std::invalid_argument(
            fmt::format(FMT_STRING("invalid '{}'"), item.first));
    }
    return item.second->as<std::string>()->get();
}

template <typename T>
std::vector<T>
readArray(ConfigItem const& item)
{
    auto result = std::vector<T>{};
    if (!item.second->is_array())
    {
        throw std::invalid_argument(
            fmt::format(FMT_STRING("'{}' must be an array"), item.first));
    }
    for (auto v : item.second->as_array()->get())
    {
        if (!v->as<T>())
        {
            throw std::invalid_argument(
                fmt::format(FMT_STRING("invalid element of '{}'"), item.first));
        }
        result.push_back(v->as<T>()->get());
    }
    return result;
}

template <typename T>
std::enable_if_t<std::is_signed_v<T>, T>
castInt(int64_t v, std::string const& name, T min, T max)
{
    if (v < min || v > max)
    {
        throw std::invalid_argument(fmt::format(FMT_STRING("bad '{}'"), name));
    }
    return static_cast<T>(v);
}

template <typename T>
std::enable_if_t<std::is_unsigned_v<T>, T>
castInt(int64_t v, std::string const& name, T min, T max)
{
    if (v < 0)
    {
        throw std::invalid_argument(fmt::format(FMT_STRING("bad '{}'"), name));
    }
    else
    {
        if (static_cast<T>(v) < min || static_cast<T>(v) > max)
        {
            throw std::invalid_argument(
                fmt::format(FMT_STRING("bad '{}'"), name));
        }
    }
    return static_cast<T>(v);
}

template <typename T>
T
readInt(ConfigItem const& item, T min = std::numeric_limits<T>::min(),
        T max = std::numeric_limits<T>::max())
{
    if (!item.second->as<int64_t>())
    {
        throw std::invalid_argument(
            fmt::format(FMT_STRING("invalid '{}'"), item.first));
    }
    return castInt<T>(item.second->as<int64_t>()->get(), item.first, min, max);
}

template <typename T>
std::vector<T>
readIntArray(ConfigItem const& item, T min = std::numeric_limits<T>::min(),
             T max = std::numeric_limits<T>::max())
{
    auto resultInt64 = readArray<int64_t>(item);
    auto result = std::vector<T>{};
    result.reserve(resultInt64.size());
    std::transform(
        resultInt64.begin(), resultInt64.end(), std::back_inserter(result),
        [&](int64_t v) { return castInt<T>(v, item.first, min, max); });
    return result;
}

template <typename T>
std::vector<T>
readXdrEnumArray(ConfigItem const& item)
{
    UnorderedMap<std::string, T> enumNames;
    for (auto enumVal : xdr::xdr_traits<T>::enum_values())
    {
        auto enumNameCharPtr =
            xdr::xdr_traits<T>::enum_name(static_cast<T>(enumVal));
        releaseAssert(enumNameCharPtr);
        enumNames.emplace(enumNameCharPtr, static_cast<T>(enumVal));
    }

    std::vector<T> result;
    if (!item.second->is_array())
    {
        throw std::invalid_argument(
            fmt::format(FMT_STRING("'{}' must be an array"), item.first));
    }
    for (auto v : item.second->as_array()->get())
    {
        if (!v->as<std::string>())
        {
            throw std::invalid_argument(
                fmt::format(FMT_STRING("invalid element of '{}'"), item.first));
        }

        auto name = v->as<std::string>()->get();
        if (enumNames.find(name) == enumNames.end())
        {
            throw std::invalid_argument(
                fmt::format(FMT_STRING("invalid element of '{}'"), item.first));
        }
        result.push_back(enumNames[name]);
    }
    return result;
}
}

void
Config::loadQset(std::shared_ptr<cpptoml::table> group, SCPQuorumSet& qset,
                 uint32 level)
{
    if (!group)
    {
        throw std::invalid_argument("invalid entry in quorum set definition");
    }

    if (level > MAXIMUM_QUORUM_NESTING_LEVEL)
    {
        throw std::invalid_argument("too many levels in quorum set");
    }

    int thresholdPercent = 67;
    qset.threshold = 0;

    for (auto& item : *group)
    {
        if (item.first == "THRESHOLD_PERCENT")
        {
            if (!item.second->as<int64_t>())
            {
                throw std::invalid_argument("invalid THRESHOLD_PERCENT");
            }
            int64_t f = item.second->as<int64_t>()->get();
            if (f <= 0 || f > 100)
            {
                throw std::invalid_argument("invalid THRESHOLD_PERCENT");
            }
            thresholdPercent = (uint32_t)f;
        }
        else if (item.first == "VALIDATORS")
        {
            auto values = readArray<std::string>(item);
            for (auto v : values)
            {
                PublicKey nodeID;
                parseNodeID(v, nodeID);
                qset.validators.emplace_back(nodeID);
            }
        }
        else
        { // must be a subset
            try
            {
                if (!item.second->is_table())
                {
                    throw std::invalid_argument(
                        "invalid quorum set, should be a group");
                }
                qset.innerSets.resize((uint32_t)qset.innerSets.size() + 1);
                loadQset(item.second->as_table(),
                         qset.innerSets[qset.innerSets.size() - 1], level + 1);
            }
            catch (std::exception& e)
            {
                std::string s;
                s = e.what();
                s += " while parsing '" + item.first + "'";
                throw std::invalid_argument(s);
            }
        }
    }

    // round up: n*percent/100
    qset.threshold = uint32(
        1 +
        (((qset.validators.size() + qset.innerSets.size()) * thresholdPercent -
          1) /
         100));

    if (qset.threshold == 0 ||
        (qset.validators.empty() && qset.innerSets.empty()))
    {
        throw std::invalid_argument("invalid quorum set definition");
    }
}

void
Config::addHistoryArchive(std::string const& name, std::string const& get,
                          std::string const& put, std::string const& mkdir)
{
    auto r = HISTORY.insert(std::make_pair(
        name, HistoryArchiveConfiguration{name, get, put, mkdir}));
    if (!r.second)
    {
        throw std::invalid_argument(
            fmt::format(FMT_STRING("Conflicting archive name '{}'"), name));
    }
}

static std::array<std::string, 4> const kQualities = {"LOW", "MEDIUM", "HIGH",
                                                      "CRITICAL"};

std::string
Config::toString(ValidatorQuality q) const
{
    return kQualities[static_cast<int>(q)];
}

Config::ValidatorQuality
Config::parseQuality(std::string const& q) const
{
    auto it = std::find(kQualities.begin(), kQualities.end(), q);

    ValidatorQuality res;

    if (it != kQualities.end())
    {
        res = static_cast<Config::ValidatorQuality>(
            std::distance(kQualities.begin(), it));
    }
    else
    {
        throw std::invalid_argument(
            fmt::format(FMT_STRING("Unknown QUALITY '{}'"), q));
    }
    return res;
}

std::vector<Config::ValidatorEntry>
Config::parseValidators(
    std::shared_ptr<cpptoml::base> validators,
    UnorderedMap<std::string, ValidatorQuality> const& domainQualityMap)
{
    std::vector<ValidatorEntry> res;

    auto tarr = validators->as_table_array();
    if (!tarr)
    {
        throw std::invalid_argument("malformed VALIDATORS");
    }
    for (auto const& valRaw : *tarr)
    {
        auto validator = valRaw->as_table();
        if (!validator)
        {
            throw std::invalid_argument("malformed VALIDATORS");
        }
        ValidatorEntry ve;
        std::string pubKey, hist;
        bool qualitySet = false;
        std::string address;
        for (auto const& f : *validator)
        {
            if (f.first == "NAME")
            {
                ve.mName = readString(f);
            }
            else if (f.first == "HOME_DOMAIN")
            {
                ve.mHomeDomain = readString(f);
            }
            else if (f.first == "QUALITY")
            {
                auto q = readString(f);
                ve.mQuality = parseQuality(q);
                qualitySet = true;
            }
            else if (f.first == "PUBLIC_KEY")
            {
                pubKey = readString(f);
            }
            else if (f.first == "ADDRESS")
            {
                address = readString(f);
            }
            else if (f.first == "HISTORY")
            {
                hist = readString(f);
            }
            else
            {
                throw std::invalid_argument(fmt::format(
                    FMT_STRING(
                        "malformed VALIDATORS entry, unknown element '{}'"),
                    f.first));
            }
        }
        if (ve.mName.empty())
        {
            throw std::invalid_argument(
                "malformed VALIDATORS entry: missing 'NAME'");
        }
        if (pubKey.empty() || ve.mHomeDomain.empty())
        {
            throw std::invalid_argument(fmt::format(
                FMT_STRING("malformed VALIDATORS entry '{}'"), ve.mName));
        }
        auto globQualityIt = domainQualityMap.find(ve.mHomeDomain);
        if (globQualityIt != domainQualityMap.end())
        {
            if (qualitySet)
            {
                throw std::invalid_argument(fmt::format(
                    FMT_STRING("malformed VALIDATORS entry '{}': quality "
                               "already defined in home domain '{}'"),
                    ve.mName, ve.mHomeDomain));
            }
            else
            {
                ve.mQuality = globQualityIt->second;
                qualitySet = true;
            }
        }
        if (!qualitySet)
        {
            throw std::invalid_argument(fmt::format(
                FMT_STRING("malformed VALIDATORS entry '{}' (missing quality)"),
                ve.mName));
        }
        addValidatorName(pubKey, ve.mName);
        ve.mKey = KeyUtils::fromStrKey<PublicKey>(pubKey);
        ve.mHasHistory = !hist.empty();
        if (ve.mHasHistory)
        {
            addHistoryArchive(ve.mName, hist, "", "");
        }
        if ((ve.mQuality == ValidatorQuality::VALIDATOR_HIGH_QUALITY ||
             ve.mQuality == ValidatorQuality::VALIDATOR_CRITICAL_QUALITY) &&
            hist.empty())
        {
            throw std::invalid_argument(fmt::format(
                FMT_STRING("malformed VALIDATORS entry '{}' (critical and "
                           "high quality must have an archive)"),
                ve.mName));
        }
        if (!address.empty())
        {
            if (NODE_HOME_DOMAIN == ve.mHomeDomain)
            {
                PREFERRED_PEERS.emplace_back(address);
            }
            else
            {
                KNOWN_PEERS.emplace_back(address);
            }
        }
        res.emplace_back(ve);
    }
    return res;
}

UnorderedMap<std::string, Config::ValidatorQuality>
Config::parseDomainsQuality(std::shared_ptr<cpptoml::base> domainsQuality)
{
    UnorderedMap<std::string, ValidatorQuality> res;
    auto tarr = domainsQuality->as_table_array();
    if (!tarr)
    {
        throw std::invalid_argument("malformed HOME_DOMAINS");
    }
    for (auto const& valRaw : *tarr)
    {
        auto home_domain = valRaw->as_table();
        if (!home_domain)
        {
            throw std::invalid_argument("malformed HOME_DOMAINS");
        }
        std::string domain;
        ValidatorQuality quality;
        bool qualitySet = false;
        for (auto const& f : *home_domain)
        {
            if (f.first == "QUALITY")
            {
                auto q = readString(f);
                quality = parseQuality(q);
                qualitySet = true;
            }
            else if (f.first == "HOME_DOMAIN")
            {
                domain = readString(f);
            }
            else
            {
                throw std::invalid_argument(fmt::format(
                    FMT_STRING("Unknown field '{}' in HOME_DOMAINS"), f.first));
            }
        }
        if (!qualitySet || domain.empty())
        {
            throw std::invalid_argument(
                fmt::format(FMT_STRING("Malformed HOME_DOMAINS '{}'"), domain));
        }
        auto p = res.emplace(std::make_pair(domain, quality));
        if (!p.second)
        {
            throw std::invalid_argument(fmt::format(
                FMT_STRING("Malformed HOME_DOMAINS: duplicate '{}'"), domain));
        }
    }
    return res;
}

void
Config::load(std::string const& filename)
{
    if (filename != Config::STDIN_SPECIAL_NAME && !fs::exists(filename))
    {
        std::string s;
        s = "No config file ";
        s += filename + " found";
        throw std::invalid_argument(s);
    }

    LOG_DEBUG(DEFAULT_LOG, "Loading config from: {}", filename);
    try
    {
        if (filename == Config::STDIN_SPECIAL_NAME)
        {
            load(std::cin);
        }
        else
        {
            std::ifstream ifs(filename);
            if (!ifs)
            {
                throw std::runtime_error(fmt::format(
                    FMT_STRING("Error opening file '{}'"), filename));
            }
            ifs.exceptions(std::ios::badbit);
            load(ifs);
        }
    }
    catch (std::exception const& ex)
    {
        std::string err("Failed to parse '");
        err += filename;
        err += "' :";
        err += ex.what();
        throw std::invalid_argument(err);
    }
}

void
Config::load(std::istream& in)
{
    std::shared_ptr<cpptoml::table> t;
    cpptoml::parser p(in);
    t = p.parse();
    processConfig(t);
}

void
Config::addSelfToValidators(
    std::vector<ValidatorEntry>& validators,
    UnorderedMap<std::string, ValidatorQuality> const& domainQualityMap)
{
    if (NODE_HOME_DOMAIN.empty())
    {
        throw std::invalid_argument("NODE_HOME_DOMAIN must be set");
    }

    ValidatorEntry self;
    self.mKey = NODE_SEED.getPublicKey();
    self.mHomeDomain = NODE_HOME_DOMAIN;
    self.mName = "self";
    self.mHasHistory = false;

    auto it = domainQualityMap.find(NODE_HOME_DOMAIN);
    if (it != domainQualityMap.end())
    {
        self.mQuality = it->second;
    }
    else
    {
        throw std::invalid_argument(fmt::format(
            FMT_STRING("Validator configured with NODE_HOME_DOMAIN='{}' "
                       "but there is no matching HOME_DOMAINS"),
            NODE_HOME_DOMAIN));
    }
    validators.emplace_back(self);
}

void
Config::verifyHistoryValidatorsBlocking(
    std::vector<ValidatorEntry> const& validators)
{
    std::vector<NodeID> archives;
    for (auto const& v : validators)
    {
        if (v.mHasHistory)
        {
            archives.emplace_back(v.mKey);
        }
    }
    if (!LocalNode::isVBlocking(QUORUM_SET, archives))
    {
        LOG_WARNING(DEFAULT_LOG,
                    "Quorum can be reached without validators with "
                    "an archive");
        if (!UNSAFE_QUORUM)
        {
            LOG_ERROR(DEFAULT_LOG, "Potentially unsafe configuration: "
                                   "validators with known archives should be "
                                   "included in all quorums. If this is really "
                                   "what you want, set UNSAFE_QUORUM=true. Be "
                                   "sure you know what you are doing!");
            throw std::invalid_argument("SCP unsafe");
        }
    }
}

void
Config::verifyLoadGenOpCountForTestingConfigs()
{
    if (LOADGEN_OP_COUNT_FOR_TESTING.size() !=
        LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING.size())
    {
        throw std::invalid_argument("LOADGEN_OP_COUNT_FOR_TESTING and "
                                    "LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING "
                                    "must be defined together and "
                                    "must have the exact same size.");
    }

    if (LOADGEN_OP_COUNT_FOR_TESTING.empty())
    {
        return;
    }

    if (!ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING)
    {
        throw std::invalid_argument(
            "When LOADGEN_OP_COUNT_FOR_TESTING and "
            "LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING are defined "
            "ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING must be set true");
    }

    if (std::any_of(LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING.begin(),
                    LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING.end(),
                    [](uint32 i) { return i == 0; }))
    {
        throw std::invalid_argument(
            "All elements in LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING must be "
            "positive integers");
    }

    if (!std::all_of(LOADGEN_OP_COUNT_FOR_TESTING.begin(),
                     LOADGEN_OP_COUNT_FOR_TESTING.end(),
                     [](unsigned short i) { return 1 <= i && i <= 100; }))
    {
        throw std::invalid_argument(
            "All elements in LOADGEN_OP_COUNT_FOR_TESTING must be "
            "integers in [1, 100]");
    }
}

void
Config::processOpApplySleepTimeForTestingConfigs()
{
    if (OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.size() !=
        OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING.size())
    {
        throw std::invalid_argument(
            "OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING and "
            "OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING must be defined together "
            "and have the same size");
    }

    if (OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.empty())
    {
        return;
    }

    if (std::any_of(OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.begin(),
                    OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.end(),
                    [](uint32 i) { return i == 0; }))
    {
        throw std::invalid_argument(
            "All elements in OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING must be "
            "positive integers");
    }

    auto sum = std::accumulate(OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.begin(),
                               OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.end(), 0);

    for (size_t i = 0; i < OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.size(); i++)
    {
        LOG_INFO(DEFAULT_LOG, "Sleeps for {} roughly {}% of the time",
                 OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING[i],
                 100 * OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING[i] / sum);
    }
}

void
Config::processConfig(std::shared_ptr<cpptoml::table> t)
{
    auto logIfSet = [](auto& item, auto const& message) {
        if (item.second->template as<bool>())
        {
            if (item.second->template as<bool>()->get())
            {
                LOG_INFO(DEFAULT_LOG, "'{}' enabled in configuration file - {}",
                         item.first, message);
            }
        }
        else
        {
            LOG_INFO(DEFAULT_LOG, "'{}' set in configuration file - {}",
                     item.first, message);
        }
    };

    try
    {
        if (!t)
        {
            throw std::runtime_error("Could not parse toml");
        }
        std::vector<ValidatorEntry> validators;
        UnorderedMap<std::string, ValidatorQuality> domainQualityMap;

        // cpptoml returns the items in non-deterministic order
        // so we need to process items that are potential dependencies first
        for (auto& item : *t)
        {
            LOG_DEBUG(DEFAULT_LOG, "Config item: {}", item.first);
            if (TESTING_ONLY_OPTIONS.count(item.first) > 0)
            {
                logIfSet(item,
                         "node will not function properly with most networks");
            }
            else if (TESTING_SUGGESTED_OPTIONS.count(item.first) > 0)
            {
                logIfSet(item,
                         "node may not function properly with most networks");
            }

            if (item.first == "PEER_READING_CAPACITY")
            {
                PEER_READING_CAPACITY = readInt<uint32_t>(item, 1);
            }
            else if (item.first == "PEER_FLOOD_READING_CAPACITY")
            {
                PEER_FLOOD_READING_CAPACITY = readInt<uint32_t>(item, 1);
            }
            else if (item.first == "FLOW_CONTROL_SEND_MORE_BATCH_SIZE")
            {
                FLOW_CONTROL_SEND_MORE_BATCH_SIZE = readInt<uint32_t>(item, 1);
            }
            else if (item.first == "PEER_FLOOD_READING_CAPACITY_BYTES")
            {
                PEER_FLOOD_READING_CAPACITY_BYTES = readInt<uint32_t>(item, 1);
            }
            else if (item.first == "FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES")
            {
                FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES =
                    readInt<uint32_t>(item, 1);
            }
            else if (item.first == "ENABLE_FLOW_CONTROL_BYTES")
            {
                ENABLE_FLOW_CONTROL_BYTES = readBool(item);
            }
            else if (item.first == "OUTBOUND_TX_QUEUE_BYTE_LIMIT")
            {
                OUTBOUND_TX_QUEUE_BYTE_LIMIT = readInt<uint32_t>(item, 1);
            }
            else if (item.first == "PEER_PORT")
            {
                PEER_PORT = readInt<unsigned short>(item, 1);
            }
            else if (item.first == "HTTP_PORT")
            {
                HTTP_PORT = readInt<unsigned short>(item);
            }
            else if (item.first == "HTTP_MAX_CLIENT")
            {
                HTTP_MAX_CLIENT = readInt<unsigned short>(item, 0);
            }
            else if (item.first == "PUBLIC_HTTP_PORT")
            {
                PUBLIC_HTTP_PORT = readBool(item);
            }
            else if (item.first == "FAILURE_SAFETY")
            {
                FAILURE_SAFETY = readInt<int32_t>(item, -1, INT32_MAX - 1);
            }
            else if (item.first == "UNSAFE_QUORUM")
            {
                UNSAFE_QUORUM = readBool(item);
            }
            else if (item.first == "LIMIT_TX_QUEUE_SOURCE_ACCOUNT")
            {
                LIMIT_TX_QUEUE_SOURCE_ACCOUNT = readBool(item);
            }
            else if (item.first == "DISABLE_XDR_FSYNC")
            {
                DISABLE_XDR_FSYNC = readBool(item);
            }
            else if (item.first == "METADATA_OUTPUT_STREAM")
            {
                METADATA_OUTPUT_STREAM = readString(item);
            }
            else if (item.first == "EXPERIMENTAL_PRECAUTION_DELAY_META")
            {
                EXPERIMENTAL_PRECAUTION_DELAY_META = readBool(item);
            }
            else if (item.first == "EXPERIMENTAL_BUCKETLIST_DB")
            {
                EXPERIMENTAL_BUCKETLIST_DB = readBool(item);
            }
            else if (item.first ==
                     "EXPERIMENTAL_BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT")
            {
                EXPERIMENTAL_BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT =
                    readInt<size_t>(item);
            }
            else if (item.first == "EXPERIMENTAL_BUCKETLIST_DB_INDEX_CUTOFF")
            {
                EXPERIMENTAL_BUCKETLIST_DB_INDEX_CUTOFF = readInt<size_t>(item);
            }
            else if (item.first == "EXPERIMENTAL_BUCKETLIST_DB_PERSIST_INDEX")
            {
                EXPERIMENTAL_BUCKETLIST_DB_PERSIST_INDEX = readBool(item);
            }
            else if (item.first == "METADATA_DEBUG_LEDGERS")
            {
                METADATA_DEBUG_LEDGERS = readInt<uint32_t>(item);
            }
            else if (item.first == "KNOWN_CURSORS")
            {
                KNOWN_CURSORS = readArray<std::string>(item);
                for (auto const& c : KNOWN_CURSORS)
                {
                    if (!ExternalQueue::validateResourceID(c))
                    {
                        throw std::invalid_argument(fmt::format(
                            FMT_STRING("invalid cursor: \"{}\""), c));
                    }
                }
            }
            else if (item.first == "RUN_STANDALONE")
            {
                RUN_STANDALONE = readBool(item);
            }
            else if (item.first == "CATCHUP_COMPLETE")
            {
                CATCHUP_COMPLETE = readBool(item);
            }
            else if (item.first == "CATCHUP_RECENT")
            {
                CATCHUP_RECENT = readInt<uint32_t>(item, 0, UINT32_MAX - 1);
            }
            else if (item.first == "ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING")
            {
                ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = readBool(item);
            }
            else if (item.first == "ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING")
            {
                ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = readBool(item);
            }
            else if (item.first == "ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING")
            {
                ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING =
                    readInt<uint32_t>(item, 0, UINT32_MAX - 1);
            }
            else if (item.first == "MAX_SLOTS_TO_REMEMBER")
            {
                MAX_SLOTS_TO_REMEMBER = readInt<uint32>(item);
            }
            else if (item.first ==
                     "ARTIFICIALLY_REPLAY_WITH_NEWEST_BUCKET_LOGIC_FOR_TESTING")
            {
                ARTIFICIALLY_REPLAY_WITH_NEWEST_BUCKET_LOGIC_FOR_TESTING =
                    readBool(item);
            }
            else if (item.first ==
                     "ARTIFICIALLY_DELAY_BUCKET_APPLICATION_FOR_TESTING")
            {
                ARTIFICIALLY_DELAY_BUCKET_APPLICATION_FOR_TESTING =
                    std::chrono::seconds(readInt<uint32_t>(item));
            }
            else if (item.first == "ALLOW_LOCALHOST_FOR_TESTING")
            {
                ALLOW_LOCALHOST_FOR_TESTING = readBool(item);
            }
            else if (item.first == "AUTOMATIC_MAINTENANCE_PERIOD")
            {
                AUTOMATIC_MAINTENANCE_PERIOD =
                    std::chrono::seconds{readInt<uint32_t>(item)};
            }
            else if (item.first == "AUTOMATIC_MAINTENANCE_COUNT")
            {
                AUTOMATIC_MAINTENANCE_COUNT = readInt<uint32_t>(item);
            }
            else if (item.first == "AUTOMATIC_SELF_CHECK_PERIOD")
            {
                AUTOMATIC_SELF_CHECK_PERIOD =
                    std::chrono::seconds{readInt<uint32_t>(item)};
            }
            else if (item.first == "MANUAL_CLOSE")
            {
                MANUAL_CLOSE = readBool(item);
            }
            else if (item.first == "LOG_FILE_PATH")
            {
                LOG_FILE_PATH = readString(item);
            }
            else if (item.first == "LOG_COLOR")
            {
                LOG_COLOR = readBool(item);
            }
            else if (item.first == "BUCKET_DIR_PATH")
            {
                BUCKET_DIR_PATH = readString(item);
            }
            else if (item.first == "NODE_NAMES")
            {
                auto names = readArray<std::string>(item);
                for (auto v : names)
                {
                    PublicKey nodeID;
                    parseNodeID(v, nodeID);
                }
            }
            else if (item.first == "NODE_SEED")
            {
                PublicKey nodeID;
                parseNodeID(readString(item), nodeID, NODE_SEED, true);
            }
            else if (item.first == "NODE_IS_VALIDATOR")
            {
                NODE_IS_VALIDATOR = readBool(item);
            }
            else if (item.first == "NODE_HOME_DOMAIN")
            {
                NODE_HOME_DOMAIN = readString(item);
            }
            else if (item.first == "TARGET_PEER_CONNECTIONS")
            {
                TARGET_PEER_CONNECTIONS = readInt<unsigned short>(item, 1);
            }
            else if (item.first == "MAX_ADDITIONAL_PEER_CONNECTIONS")
            {
                MAX_ADDITIONAL_PEER_CONNECTIONS = readInt<int>(
                    item, -1, std::numeric_limits<unsigned short>::max());
            }
            else if (item.first == "MAX_PENDING_CONNECTIONS")
            {
                MAX_PENDING_CONNECTIONS = readInt<unsigned short>(
                    item, 1, std::numeric_limits<unsigned short>::max());
            }
            else if (item.first == "PEER_AUTHENTICATION_TIMEOUT")
            {
                PEER_AUTHENTICATION_TIMEOUT = readInt<unsigned short>(
                    item, 1, std::numeric_limits<unsigned short>::max());
            }
            else if (item.first == "PEER_TIMEOUT")
            {
                PEER_TIMEOUT = readInt<unsigned short>(
                    item, 1, std::numeric_limits<unsigned short>::max());
            }
            else if (item.first == "PEER_STRAGGLER_TIMEOUT")
            {
                PEER_STRAGGLER_TIMEOUT = readInt<unsigned short>(
                    item, 1, std::numeric_limits<unsigned short>::max());
            }
            else if (item.first == "MAX_BATCH_WRITE_COUNT")
            {
                MAX_BATCH_WRITE_COUNT = readInt<int>(item, 1);
            }
            else if (item.first == "MAX_BATCH_WRITE_BYTES")
            {
                MAX_BATCH_WRITE_BYTES = readInt<int>(item, 1);
            }
            else if (item.first == "FLOOD_OP_RATE_PER_LEDGER")
            {
                FLOOD_OP_RATE_PER_LEDGER = readDouble(item);
                if (FLOOD_OP_RATE_PER_LEDGER <= 0.0)
                {
                    throw std::invalid_argument(
                        "bad value for FLOOD_OP_RATE_PER_LEDGER");
                }
            }
            else if (item.first == "FLOOD_TX_PERIOD_MS")
            {
                FLOOD_TX_PERIOD_MS = readInt<int>(item, 1);
            }
            else if (item.first == "FLOOD_DEMAND_PERIOD_MS")
            {
                FLOOD_DEMAND_PERIOD_MS =
                    std::chrono::milliseconds(readInt<int>(item, 1));
            }
            else if (item.first == "FLOOD_ADVERT_PERIOD_MS")
            {
                FLOOD_ADVERT_PERIOD_MS =
                    std::chrono::milliseconds(readInt<int>(item, 1));
            }
            else if (item.first == "FLOOD_DEMAND_BACKOFF_DELAY_MS")
            {
                FLOOD_DEMAND_BACKOFF_DELAY_MS =
                    std::chrono::milliseconds(readInt<int>(item, 1));
            }
            else if (item.first == "FLOOD_ARB_TX_BASE_ALLOWANCE")
            {
                FLOOD_ARB_TX_BASE_ALLOWANCE = readInt<int32_t>(item, -1);
            }
            else if (item.first == "FLOOD_ARB_TX_DAMPING_FACTOR")
            {
                FLOOD_ARB_TX_DAMPING_FACTOR = readDouble(item);
                if (FLOOD_ARB_TX_DAMPING_FACTOR <= 0.0 ||
                    FLOOD_ARB_TX_DAMPING_FACTOR > 1.0)
                {
                    throw std::invalid_argument(
                        "bad value for FLOOD_ARB_TX_DAMPING_FACTOR");
                }
            }
            else if (item.first == "PREFERRED_PEERS")
            {
                PREFERRED_PEERS = readArray<std::string>(item);
            }
            else if (item.first == "PREFERRED_PEER_KEYS")
            {
                // handled below
            }
            else if (item.first == "PREFERRED_PEERS_ONLY")
            {
                PREFERRED_PEERS_ONLY = readBool(item);
            }
            else if (item.first == "KNOWN_PEERS")
            {
                auto peers = readArray<std::string>(item);
                KNOWN_PEERS.insert(KNOWN_PEERS.begin(), peers.begin(),
                                   peers.end());
            }
            else if (item.first == "QUORUM_SET")
            {
                // processing performed after this loop
            }
            else if (item.first == "COMMANDS")
            {
                COMMANDS = readArray<std::string>(item);
            }
            else if (item.first == "WORKER_THREADS")
            {
                WORKER_THREADS = readInt<int>(item, 1, 1000);
            }
            else if (item.first == "MAX_CONCURRENT_SUBPROCESSES")
            {
                MAX_CONCURRENT_SUBPROCESSES = readInt<size_t>(item, 1);
            }
            else if (item.first == "QUORUM_INTERSECTION_CHECKER")
            {
                QUORUM_INTERSECTION_CHECKER = readBool(item);
            }
            else if (item.first == "HISTORY")
            {
                auto hist = item.second->as_table();
                if (hist)
                {
                    for (auto const& archive : *hist)
                    {
                        LOG_DEBUG(DEFAULT_LOG, "History archive: {}",
                                  archive.first);
                        auto tab = archive.second->as_table();
                        if (!tab)
                        {
                            throw std::invalid_argument(
                                "malformed HISTORY config block");
                        }
                        std::string get, put, mkdir;
                        for (auto const& c : *tab)
                        {
                            if (c.first == "get")
                            {
                                get = c.second->as<std::string>()->get();
                            }
                            else if (c.first == "put")
                            {
                                put = c.second->as<std::string>()->get();
                            }
                            else if (c.first == "mkdir")
                            {
                                mkdir = c.second->as<std::string>()->get();
                            }
                            else
                            {
                                std::string err(
                                    "Unknown HISTORY-table entry: '");
                                err += c.first;
                                err +=
                                    "', within [HISTORY." + archive.first + "]";
                                throw std::invalid_argument(err);
                            }
                        }
                        addHistoryArchive(archive.first, get, put, mkdir);
                    }
                }
                else
                {
                    throw std::invalid_argument("incomplete HISTORY block");
                }
            }
            else if (item.first == "DATABASE")
            {
                DATABASE = SecretValue{readString(item)};
            }
            else if (item.first == "NETWORK_PASSPHRASE")
            {
                NETWORK_PASSPHRASE = readString(item);
            }
            else if (item.first == "INVARIANT_CHECKS")
            {
                INVARIANT_CHECKS = readArray<std::string>(item);
            }
            else if (item.first == "ENTRY_CACHE_SIZE")
            {
                ENTRY_CACHE_SIZE = readInt<uint32_t>(item);
            }
            else if (item.first == "PREFETCH_BATCH_SIZE")
            {
                PREFETCH_BATCH_SIZE = readInt<uint32_t>(item);
            }
            else if (item.first == "MAXIMUM_LEDGER_CLOSETIME_DRIFT")
            {
                MAXIMUM_LEDGER_CLOSETIME_DRIFT = readInt<int64_t>(item, 0);
            }
            else if (item.first == "VALIDATORS")
            {
                // processed later (may depend on HOME_DOMAINS)
            }
            else if (item.first == "HOME_DOMAINS")
            {
                domainQualityMap = parseDomainsQuality(item.second);
            }
            else if (item.first == "SURVEYOR_KEYS")
            {
                // processed later (may depend on previously defined public
                // keys)
            }
            else if (item.first ==
                     "EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE")
            {
                EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE =
                    readXdrEnumArray<OperationType>(item);
            }
            else if (item.first == "OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING")
            {
                // Since it doesn't make sense to sleep for a negative amount of
                // time, we use an unsigned integer type.
                auto input = readIntArray<uint32>(item);
                OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING.reserve(input.size());
                // Convert uint32 to std::chrono::microseconds
                std::transform(
                    input.begin(), input.end(),
                    std::back_inserter(
                        OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING),
                    [](uint32 x) { return std::chrono::microseconds(x); });
            }
            else if (item.first == "OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING")
            {
                OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING =
                    readIntArray<uint32>(item);
            }
            else if (item.first == "LOADGEN_OP_COUNT_FOR_TESTING")
            {
                LOADGEN_OP_COUNT_FOR_TESTING =
                    readIntArray<unsigned short>(item);
            }
            else if (item.first == "LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING")
            {
                LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING =
                    readIntArray<uint32>(item);
            }
            else if (item.first == "CATCHUP_WAIT_MERGES_TX_APPLY_FOR_TESTING")
            {
                CATCHUP_WAIT_MERGES_TX_APPLY_FOR_TESTING = readBool(item);
            }
            else if (item.first == "HISTOGRAM_WINDOW_SIZE")
            {
                auto const s = readInt<uint32_t>(item);
                // 5 minutes is hardcoded in many places in prometheus.
                // Thus the window size should divide it evenly.
                if (300 % s != 0)
                {
                    throw std::invalid_argument(
                        "HISTOGRAM_WINDOW_SIZE must divide 300 evenly");
                }
                HISTOGRAM_WINDOW_SIZE = std::chrono::seconds(s);
            }
            else if (item.first == "HALT_ON_INTERNAL_TRANSACTION_ERROR")
            {
                HALT_ON_INTERNAL_TRANSACTION_ERROR = readBool(item);
            }
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
            else if (item.first == "ENABLE_SOROBAN_DIAGNOSTIC_EVENTS")
            {
                ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = readBool(item);
            }
#endif
            else if (item.first == "ARTIFICIALLY_SLEEP_MAIN_THREAD_FOR_TESTING")
            {
                ARTIFICIALLY_SLEEP_MAIN_THREAD_FOR_TESTING =
                    std::chrono::microseconds(readInt<uint32_t>(item));
            }
            else if (item.first == "MAX_DEX_TX_OPERATIONS_IN_TX_SET")
            {
                auto value = readInt<uint32_t>(item);
                if (value > 0 && value < MAX_OPS_PER_TX + 2)
                {
                    throw std::invalid_argument(fmt::format(
                        "MAX_DEX_TX_OPERATIONS_IN_TX_SET must be either 0 or "
                        "at least {} in order to not drop any transactions.",
                        MAX_OPS_PER_TX + 2));
                }
                MAX_DEX_TX_OPERATIONS_IN_TX_SET =
                    value == 0 ? std::nullopt : std::make_optional(value);
            }
            else
            {
                std::string err("Unknown configuration entry: '");
                err += item.first;
                err += "'";
                throw std::invalid_argument(err);
            }
        }

        if (!OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING.empty() ||
            !OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.empty())
        {
            processOpApplySleepTimeForTestingConfigs();
        }

        if (FLOW_CONTROL_SEND_MORE_BATCH_SIZE > PEER_FLOOD_READING_CAPACITY)
        {
            std::string msg =
                "Invalid configuration: FLOW_CONTROL_SEND_MORE_BATCH_SIZE "
                "can't be greater than PEER_FLOOD_READING_CAPACITY";
            throw std::runtime_error(msg);
        }

        if (FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES >
            PEER_FLOOD_READING_CAPACITY_BYTES)
        {
            std::string msg =
                "Invalid configuration: "
                "FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES "
                "can't be greater than PEER_FLOOD_READING_CAPACITY_BYTES";
            throw std::runtime_error(msg);
        }

        // PEER_FLOOD_READING_CAPACITY_BYTES (C): This is the initial credit
        // given to the sender. It is the maximum number of bytes that the
        // sender can transmit to the receiver before it needs to wait for
        // an acknowledgement from the receiver. It represents the initial
        // 'capacity' of the connection.

        // MAX_CLASSIC_TX_SIZE_BYTES (M): This is the maximum size, in bytes, of
        // a single message that can be sent by the sender. The sender can send
        // messages of any size up to this limit, provided it has enough credit.

        // FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES (A): This is the number of
        // bytes that the receiver must process before it sends an
        // acknowledgement back to the sender. The acknowledgement also serves
        // to replenish the sender's credit by this amount, enabling it to send
        // more data.

        // The relationship between these three parameters should satisfy: C - A
        // >= M. This ensures that the sender can always continue sending
        // messages until it receives an acknowledgement for the previous data,
        // thus preventing the system from getting stuck.

        // Start with initial PEER_FLOOD_READING_CAPACITY_BYTES (C) credit
        // Sender (C) -------- M1 bytes ----------> Receiver
        //          \-- C-M1 --/

        // Receiver processes received bytes and once
        // FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES (A) or more is processed, an
        // acknowledgement is sent, which replenishes the sender's credit
        // Sender (C-M1+A) <-- A bytes ---------- Receiver
        //             \--- (C-M1+A)-M2 --->/

        // Note:  M1, M2... are message sizes such that M <=
        // MAX_CLASSIC_TX_SIZE_BYTES
        if (!(PEER_FLOOD_READING_CAPACITY_BYTES -
                  FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES >=
              MAX_CLASSIC_TX_SIZE_BYTES))
        {
            std::string msg =
                "Invalid configuration: the difference between "
                "PEER_FLOOD_READING_CAPACITY_BYTES and "
                "FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES must be at least "
                "(MAX_CLASSIC_TX_SIZE_BYTES)";
            throw std::runtime_error(msg);
        }

        verifyLoadGenOpCountForTestingConfigs();

        gIsProductionNetwork = NETWORK_PASSPHRASE ==
                               "Public Global Stellar Network ; September 2015";

        // Validators default to starting the network from local state
        FORCE_SCP = NODE_IS_VALIDATOR;

        // process elements that potentially depend on others
        if (t->contains("VALIDATORS"))
        {
            auto vals = t->get("VALIDATORS");
            if (vals)
            {
                validators = parseValidators(vals, domainQualityMap);
            }
        }

        // if only QUORUM_SET is specified: we don't populate validators at all
        if (NODE_IS_VALIDATOR &&
            !(validators.empty() && t->contains("QUORUM_SET")))
        {
            addSelfToValidators(validators, domainQualityMap);
        }

        parseNodeIDsIntoSet(t, "PREFERRED_PEER_KEYS", PREFERRED_PEER_KEYS);
        parseNodeIDsIntoSet(t, "SURVEYOR_KEYS", SURVEYOR_KEYS);

        auto autoQSet = generateQuorumSet(validators);
        auto autoQSetStr = toString(autoQSet);
        ValidationThresholdLevels thresholdLevel;

        if (t->contains("QUORUM_SET"))
        {
            auto qset = t->get("QUORUM_SET");
            if (qset)
            {
                loadQset(qset->as_table(), QUORUM_SET, 0);
            }
            auto s = toString(QUORUM_SET);
            LOG_INFO(DEFAULT_LOG, "Using QUORUM_SET: {}", s);
            if (s != autoQSetStr && !validators.empty())
            {
                LOG_WARNING(DEFAULT_LOG, "Differs from generated: {}",
                            autoQSetStr);
                if (!UNSAFE_QUORUM)
                {
                    LOG_ERROR(DEFAULT_LOG,
                              "Can't override [[VALIDATORS]] with "
                              "QUORUM_SET unless you also set "
                              "UNSAFE_QUORUM=true. Be sure you know what "
                              "you are doing!");
                    throw std::invalid_argument("SCP unsafe");
                }
            }
            thresholdLevel = ValidationThresholdLevels::
                BYZANTINE_FAULT_TOLERANCE; // assume validators are from
                                           // different entities
        }
        else
        {
            LOG_INFO(DEFAULT_LOG, "Generated QUORUM_SET: {}", autoQSetStr);
            QUORUM_SET = autoQSet;
            verifyHistoryValidatorsBlocking(validators);
            // count the number of domains
            UnorderedSet<std::string> domains;
            for (auto const& v : validators)
            {
                domains.insert(v.mHomeDomain);
            }
            thresholdLevel =
                domains.size() > 1
                    ? ValidationThresholdLevels::BYZANTINE_FAULT_TOLERANCE
                    : ValidationThresholdLevels::SIMPLE_MAJORITY;
        }

        adjust();
        validateConfig(thresholdLevel);
    }
    catch (cpptoml::parse_exception& ex)
    {
        throw std::invalid_argument(ex.what());
    }
}

void
Config::adjust()
{
    if (MAX_ADDITIONAL_PEER_CONNECTIONS == -1)
    {
        if (TARGET_PEER_CONNECTIONS <=
            std::numeric_limits<unsigned short>::max() / 8)
        {
            MAX_ADDITIONAL_PEER_CONNECTIONS = TARGET_PEER_CONNECTIONS * 8;
        }
        else
        {
            MAX_ADDITIONAL_PEER_CONNECTIONS =
                std::numeric_limits<unsigned short>::max();
        }
    }

    // Ensure outbound connections are capped based on inbound rate
    int limit =
        MAX_ADDITIONAL_PEER_CONNECTIONS / OverlayManager::MIN_INBOUND_FACTOR +
        OverlayManager::MIN_INBOUND_FACTOR;
    if (static_cast<int>(TARGET_PEER_CONNECTIONS) > limit)
    {
        TARGET_PEER_CONNECTIONS = static_cast<unsigned short>(limit);
        LOG_WARNING(DEFAULT_LOG,
                    "Adjusted TARGET_PEER_CONNECTIONS to {} due to "
                    "insufficient MAX_ADDITIONAL_PEER_CONNECTIONS={}",
                    limit, MAX_ADDITIONAL_PEER_CONNECTIONS);
    }

    auto const originalMaxAdditionalPeerConnections =
        MAX_ADDITIONAL_PEER_CONNECTIONS;
    auto const originalTargetPeerConnections = TARGET_PEER_CONNECTIONS;
    auto const originalMaxPendingConnections = MAX_PENDING_CONNECTIONS;

    int maxFsConnections = std::min<int>(
        std::numeric_limits<unsigned short>::max(), fs::getMaxConnections());

    auto totalAuthenticatedConnections =
        TARGET_PEER_CONNECTIONS + MAX_ADDITIONAL_PEER_CONNECTIONS;

    int maxPendingConnections = MAX_PENDING_CONNECTIONS;

    if (totalAuthenticatedConnections > 0)
    {
        auto outboundPendingRate =
            double(TARGET_PEER_CONNECTIONS) / totalAuthenticatedConnections;

        auto doubleToNonzeroUnsignedShort = [](double v) {
            auto rounded = static_cast<int>(std::ceil(v));
            auto cappedToUnsignedShort = std::min<int>(
                std::numeric_limits<unsigned short>::max(), rounded);
            return static_cast<unsigned short>(
                std::max<int>(1, cappedToUnsignedShort));
        };

        // see if we need to reduce maxPendingConnections
        if (totalAuthenticatedConnections + maxPendingConnections >
            maxFsConnections)
        {
            maxPendingConnections =
                totalAuthenticatedConnections >= maxFsConnections
                    ? 1
                    : static_cast<unsigned short>(
                          maxFsConnections - totalAuthenticatedConnections);
        }

        // if we're still over, we scale everything
        if (totalAuthenticatedConnections + maxPendingConnections >
            maxFsConnections)
        {
            maxPendingConnections = std::max<int>(MAX_PENDING_CONNECTIONS, 1);

            int totalRequiredConnections =
                totalAuthenticatedConnections + maxPendingConnections;

            auto outboundRate =
                (double)TARGET_PEER_CONNECTIONS / totalRequiredConnections;
            auto inboundRate = (double)MAX_ADDITIONAL_PEER_CONNECTIONS /
                               totalRequiredConnections;

            TARGET_PEER_CONNECTIONS =
                doubleToNonzeroUnsignedShort(maxFsConnections * outboundRate);
            MAX_ADDITIONAL_PEER_CONNECTIONS =
                doubleToNonzeroUnsignedShort(maxFsConnections * inboundRate);

            auto authenticatedConnections =
                TARGET_PEER_CONNECTIONS + MAX_ADDITIONAL_PEER_CONNECTIONS;
            maxPendingConnections =
                authenticatedConnections >= maxFsConnections
                    ? 1
                    : static_cast<unsigned short>(maxFsConnections -
                                                  authenticatedConnections);
        }

        MAX_PENDING_CONNECTIONS = static_cast<unsigned short>(std::min<int>(
            std::numeric_limits<unsigned short>::max(), maxPendingConnections));

        // derive outbound/inbound pending connections
        // from MAX_PENDING_CONNECTIONS, using the ratio of inbound/outbound
        // connections
        if (MAX_OUTBOUND_PENDING_CONNECTIONS == 0 &&
            MAX_INBOUND_PENDING_CONNECTIONS == 0)
        {
            MAX_OUTBOUND_PENDING_CONNECTIONS = std::max<unsigned short>(
                1, doubleToNonzeroUnsignedShort(MAX_PENDING_CONNECTIONS *
                                                outboundPendingRate));
            MAX_INBOUND_PENDING_CONNECTIONS = std::max<unsigned short>(
                1, MAX_PENDING_CONNECTIONS - MAX_OUTBOUND_PENDING_CONNECTIONS);
        }
    }
    else
    {
        MAX_OUTBOUND_PENDING_CONNECTIONS = 0;
        MAX_INBOUND_PENDING_CONNECTIONS = 0;
    }
    auto warnIfChanged = [&](std::string const name, auto const originalValue,
                             auto const newValue) {
        if (originalValue != newValue)
        {
            LOG_WARNING(DEFAULT_LOG,
                        "Adjusted {} from {} to {} due to OS limits (the "
                        "maximum number of file descriptors)",
                        name, originalValue, newValue);
        }
    };
    warnIfChanged("MAX_ADDITIONAL_PEER_CONNECTIONS",
                  originalMaxAdditionalPeerConnections,
                  MAX_ADDITIONAL_PEER_CONNECTIONS);
    warnIfChanged("TARGET_PEER_CONNECTIONS", originalTargetPeerConnections,
                  TARGET_PEER_CONNECTIONS);
    warnIfChanged("MAX_PENDING_CONNECTIONS", originalMaxPendingConnections,
                  MAX_PENDING_CONNECTIONS);
}

void
Config::logBasicInfo()
{
    LOG_INFO(DEFAULT_LOG, "Connection effective settings:");
    LOG_INFO(DEFAULT_LOG, "TARGET_PEER_CONNECTIONS: {}",
             TARGET_PEER_CONNECTIONS);
    LOG_INFO(DEFAULT_LOG, "MAX_ADDITIONAL_PEER_CONNECTIONS: {}",
             MAX_ADDITIONAL_PEER_CONNECTIONS);
    LOG_INFO(DEFAULT_LOG, "MAX_PENDING_CONNECTIONS: {}",
             MAX_PENDING_CONNECTIONS);
    LOG_INFO(DEFAULT_LOG, "MAX_OUTBOUND_PENDING_CONNECTIONS: {}",
             MAX_OUTBOUND_PENDING_CONNECTIONS);
    LOG_INFO(DEFAULT_LOG, "MAX_INBOUND_PENDING_CONNECTIONS: {}",
             MAX_INBOUND_PENDING_CONNECTIONS);
}

void
Config::validateConfig(ValidationThresholdLevels thresholdLevel)
{
    std::set<NodeID> nodes;
    LocalNode::forAllNodes(QUORUM_SET, [&](NodeID const& n) {
        nodes.insert(n);
        return true;
    });

    if (nodes.empty())
    {
        throw std::invalid_argument(
            "no validators defined in VALIDATORS/QUORUM_SET");
    }

    // calculates nodes that would break quorum
    auto selfID = NODE_SEED.getPublicKey();
    auto r = LocalNode::findClosestVBlocking(QUORUM_SET, nodes, nullptr);

    unsigned int minSize = computeDefaultThreshold(QUORUM_SET, thresholdLevel);

    if (FAILURE_SAFETY == -1)
    {
        // calculates default value for safety giving the top level entities
        // the same weight
        auto topLevelCount = static_cast<uint32>(QUORUM_SET.validators.size() +
                                                 QUORUM_SET.innerSets.size());
        FAILURE_SAFETY = topLevelCount - minSize;

        LOG_INFO(DEFAULT_LOG,
                 "Assigning calculated value of {} to FAILURE_SAFETY",
                 FAILURE_SAFETY);
    }

    try
    {
        if (FAILURE_SAFETY >= static_cast<int32_t>(r.size()))
        {
            LOG_ERROR(DEFAULT_LOG,
                      "Not enough nodes / thresholds too strict in your "
                      "Quorum set to ensure your desired level of "
                      "FAILURE_SAFETY. Reduce FAILURE_SAFETY or fix "
                      "quorum set");
            throw std::invalid_argument(
                "FAILURE_SAFETY incompatible with QUORUM_SET");
        }

        if (!UNSAFE_QUORUM)
        {
            if (FAILURE_SAFETY == 0)
            {
                LOG_ERROR(DEFAULT_LOG,
                          "Can't have FAILURE_SAFETY=0 unless you also set "
                          "UNSAFE_QUORUM=true. Be sure you know what you are "
                          "doing!");
                throw std::invalid_argument("SCP unsafe");
            }

            if (QUORUM_SET.threshold < minSize)
            {
                LOG_ERROR(DEFAULT_LOG,
                          "Your THRESHOLD_PERCENTAGE is too low. If you "
                          "really want this set UNSAFE_QUORUM=true. Be "
                          "sure you know what you are doing!");
                throw std::invalid_argument("SCP unsafe");
            }
        }
    }
    catch (...)
    {
        LOG_INFO(DEFAULT_LOG, " Current QUORUM_SET breaks with {} failures",
                 r.size());
        throw;
    }

    const char* errString = nullptr;
    if (!isQuorumSetSane(QUORUM_SET, !UNSAFE_QUORUM, errString))
    {
        LOG_FATAL(DEFAULT_LOG, "Invalid QUORUM_SET: {}", errString);
        throw std::invalid_argument("Invalid QUORUM_SET");
    }
}

void
Config::parseNodeID(std::string configStr, PublicKey& retKey)
{
    SecretKey k;
    parseNodeID(configStr, retKey, k, false);
}

void
Config::addValidatorName(std::string const& pubKeyStr, std::string const& name)
{
    PublicKey k;
    std::string cName = "$";
    cName += name;
    if (resolveNodeID(cName, k))
    {
        throw std::invalid_argument("name already used: " + name);
    }

    if (!VALIDATOR_NAMES.emplace(std::make_pair(pubKeyStr, name)).second)
    {
        throw std::invalid_argument("naming node twice: " + name);
    }
}

void
Config::parseNodeID(std::string configStr, PublicKey& retKey, SecretKey& sKey,
                    bool isSeed)
{
    if (configStr.size() < 2)
    {
        throw std::invalid_argument("invalid key: " + configStr);
    }

    // check if configStr is a PublicKey or a common name
    if (configStr[0] == '$')
    {
        if (isSeed)
        {
            throw std::invalid_argument("aliases only store public keys: " +
                                        configStr);
        }
        if (!resolveNodeID(configStr, retKey))
        {
            throw std::invalid_argument("unknown key in config: " + configStr);
        }
    }
    else
    {
        std::istringstream iss(configStr);
        std::string nodestr;
        iss >> nodestr;
        if (isSeed)
        {
            sKey = SecretKey::fromStrKeySeed(nodestr);
            retKey = sKey.getPublicKey();
            nodestr = sKey.getStrKeyPublic();
        }
        else
        {
            retKey = KeyUtils::fromStrKey<PublicKey>(nodestr);
        }

        if (iss)
        { // get any common name they have added
            std::string commonName;
            iss >> commonName;
            if (commonName.size())
            {
                addValidatorName(nodestr, commonName);
            }
        }
    }
}

void
Config::parseNodeIDsIntoSet(std::shared_ptr<cpptoml::table> t,
                            std::string const& configStr,
                            std::set<PublicKey>& keySet)
{
    if (t->contains(configStr))
    {
        auto nodes = t->get(configStr);
        if (nodes)
        {
            auto values = readArray<std::string>(ConfigItem{configStr, nodes});
            for (auto const& v : values)
            {
                PublicKey nodeID;
                parseNodeID(v, nodeID);
                keySet.emplace(nodeID);
            }
        }
    }
}

std::string
Config::toShortString(PublicKey const& pk) const
{
    std::string ret = KeyUtils::toStrKey(pk);
    auto it = VALIDATOR_NAMES.find(ret);
    if (it == VALIDATOR_NAMES.end())
        return ret.substr(0, 5);
    else
        return it->second;
}

std::string
Config::toStrKey(PublicKey const& pk, bool fullKey) const
{
    std::string res;
    if (fullKey)
    {
        res = KeyUtils::toStrKey(pk);
    }
    else
    {
        res = toShortString(pk);
    }
    return res;
}

bool
Config::resolveNodeID(std::string const& s, PublicKey& retKey) const
{
    auto expanded = expandNodeID(s);
    if (expanded.empty())
    {
        return false;
    }

    try
    {
        retKey = KeyUtils::fromStrKey<PublicKey>(expanded);
    }
    catch (std::invalid_argument&)
    {
        return false;
    }
    return true;
}

std::string
Config::expandNodeID(const std::string& s) const
{
    if (s.length() < 2)
    {
        return s;
    }
    if (s[0] != '$' && s[0] != '@')
    {
        return s;
    }

    using validatorMatcher_t =
        std::function<bool(std::pair<std::string, std::string> const&)>;
    auto arg = s.substr(1);
    auto validatorMatcher =
        s[0] == '$'
            ? validatorMatcher_t{[&](std::pair<std::string, std::string> const&
                                         p) { return p.second == arg; }}
            : validatorMatcher_t{
                  [&](std::pair<std::string, std::string> const& p) {
                      return p.first.compare(0, arg.size(), arg) == 0;
                  }};

    auto it = std::find_if(VALIDATOR_NAMES.begin(), VALIDATOR_NAMES.end(),
                           validatorMatcher);
    if (it != VALIDATOR_NAMES.end())
    {
        return it->first;
    }
    else
    {
        return {};
    }
}

std::chrono::seconds
Config::getExpectedLedgerCloseTime() const
{
    if (ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING)
    {
        return std::chrono::seconds{ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING};
    }
    if (ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING)
    {
        return std::chrono::seconds{1};
    }
    return Herder::EXP_LEDGER_TIMESPAN_SECONDS;
}

void
Config::setInMemoryMode()
{
    MODE_USES_IN_MEMORY_LEDGER = true;
    DATABASE = SecretValue{"sqlite3://:memory:"};
    MODE_STORES_HISTORY_MISC = false;
    MODE_STORES_HISTORY_LEDGERHEADERS = false;
    MODE_ENABLES_BUCKETLIST = true;
}

bool
Config::isInMemoryMode() const
{
    return MODE_USES_IN_MEMORY_LEDGER;
}

bool
Config::isUsingBucketListDB() const
{
    return EXPERIMENTAL_BUCKETLIST_DB && !MODE_USES_IN_MEMORY_LEDGER &&
           MODE_ENABLES_BUCKETLIST;
}

bool
Config::isPersistingBucketListDBIndexes() const
{
    return isUsingBucketListDB() && EXPERIMENTAL_BUCKETLIST_DB_PERSIST_INDEX &&
           !NODE_IS_VALIDATOR;
}

bool
Config::isInMemoryModeWithoutMinimalDB() const
{
    return MODE_USES_IN_MEMORY_LEDGER && !MODE_STORES_HISTORY_LEDGERHEADERS;
}

bool
Config::modeStoresAllHistory() const
{
    return MODE_STORES_HISTORY_LEDGERHEADERS && MODE_STORES_HISTORY_MISC;
}

bool
Config::modeStoresAnyHistory() const
{
    return MODE_STORES_HISTORY_LEDGERHEADERS || MODE_STORES_HISTORY_MISC;
}

void
Config::setNoListen()
{
    // prevent opening up a port for other peers
    RUN_STANDALONE = true;
    HTTP_PORT = 0;
    MANUAL_CLOSE = true;
}

void
Config::setNoPublish()
{
    for (auto& item : HISTORY)
    {
        item.second.mPutCmd = "";
    }
}

SCPQuorumSet
Config::generateQuorumSetHelper(
    std::vector<ValidatorEntry>::const_iterator begin,
    std::vector<ValidatorEntry>::const_iterator end,
    ValidatorQuality curQuality)
{
    auto it = begin;
    SCPQuorumSet ret;
    while (it != end && it->mQuality == curQuality)
    {
        SCPQuorumSet innerSet;
        auto& vals = innerSet.validators;
        auto it2 = it;
        for (; it2 != end && it2->mHomeDomain == it->mHomeDomain; it2++)
        {
            if (it2->mQuality != it->mQuality)
            {
                throw std::invalid_argument(fmt::format(
                    FMT_STRING(
                        "Validators '{}' and '{}' must have same quality"),
                    it->mName, it2->mName));
            }
            vals.emplace_back(it2->mKey);
        }
        if (vals.size() < 3 &&
            (it->mQuality == ValidatorQuality::VALIDATOR_HIGH_QUALITY ||
             it->mQuality == ValidatorQuality::VALIDATOR_CRITICAL_QUALITY))
        {
            throw std::invalid_argument(fmt::format(
                FMT_STRING("Critical and High quality validators for '{}' "
                           "must have redundancy of at least 3"),
                it->mHomeDomain));
        }
        innerSet.threshold = computeDefaultThreshold(
            innerSet, ValidationThresholdLevels::SIMPLE_MAJORITY);

        ret.innerSets.emplace_back(innerSet);
        it = it2;
    }
    if (it != end)
    {
        if (it->mQuality > curQuality)
        {
            throw std::invalid_argument(fmt::format(
                FMT_STRING(
                    "invalid validator quality for '{}' (must be ascending)"),
                it->mName));
        }
        auto lowQ = generateQuorumSetHelper(it, end, it->mQuality);
        ret.innerSets.emplace_back(lowQ);
    }
    auto thresholdLevel =
        curQuality == ValidatorQuality::VALIDATOR_CRITICAL_QUALITY
            ? ValidationThresholdLevels::ALL_REQUIRED
            : ValidationThresholdLevels::BYZANTINE_FAULT_TOLERANCE;

    ret.threshold = computeDefaultThreshold(ret, thresholdLevel);
    return ret;
}

SCPQuorumSet
Config::generateQuorumSet(std::vector<ValidatorEntry> const& validators)
{
    auto todo = validators;
    // first, sort by quality (desc), homedomain (asc)
    std::sort(todo.begin(), todo.end(),
              [](ValidatorEntry const& l, ValidatorEntry const& r) {
                  if (l.mQuality > r.mQuality)
                  {
                      return true;
                  }
                  else if (l.mQuality < r.mQuality)
                  {
                      return false;
                  }
                  return l.mHomeDomain < r.mHomeDomain;
              });

    auto res = generateQuorumSetHelper(
        todo.begin(), todo.end(), ValidatorQuality::VALIDATOR_CRITICAL_QUALITY);
    normalizeQSet(res);
    return res;
}

std::string
Config::toString(SCPQuorumSet const& qset)
{
    auto json = LocalNode::toJson(
        qset, [&](PublicKey const& k) { return toShortString(k); });
    Json::StyledWriter fw;
    return fw.write(json);
}

std::string const Config::STDIN_SPECIAL_NAME = "stdin";
}
