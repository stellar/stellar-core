// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/RelayApplication.h"
#include "bucket/BucketManager.h"
#include "database/DatabaseImpl.h"
#include "herder/HerderImpl.h"
#include "herder/HerderPersistenceImpl.h"
#include "herder/LedgerCloseData.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryManagerImpl.h"
#include "ledger/LedgerManagerImpl.h"
#include "ledger/LedgerTxn.h"
#include "main/CommandHandlerImpl.h"
#include "main/Maintainer.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/PeerManagerImpl.h"
#include "overlay/RandomPeerSource.h"
#include "util/Math.h"
#include "work/WorkScheduler.h"

namespace
{
using namespace stellar;

class StubError : public std::runtime_error
{
  public:
    StubError() : std::runtime_error("Stub method caled")
    {
    }
};

class LedgerManagerStub : public LedgerManagerImpl
{

  public:
    LedgerManagerStub(Application& app) : LedgerManagerImpl(app)
    {
    }

    virtual void bootstrap() override{};
    virtual State
    getState() const override
    {
        return LedgerManager::State::LM_SYNCED_STATE;
    };
    virtual CatchupState
    getCatchupState() const override
    {
        return LedgerManager::CatchupState::NONE;
    };

    virtual void
    valueExternalized(LedgerCloseData const& ledgerData) override
    {
        // Produce an extremely minimal ledger chain, with no txset, bucket or
        // history processing.
        LedgerHeader header;
        header.ledgerVersion = mApp.getConfig().LEDGER_PROTOCOL_VERSION;
        header.ledgerSeq = ledgerData.getLedgerSeq();
        header.scpValue = ledgerData.getValue();
        header.previousLedgerHash = getLastClosedLedgerHeader().hash;
        advanceLedgerPointers(header);
    };
    virtual HistoryArchiveState
    getLastClosedLedgerHAS() override
    {
        return HistoryArchiveState();
    };
    virtual void
    syncMetrics() override
    {
    }
    virtual void
    startNewLedger(LedgerHeader const& genesisLedger) override
    {
        CLOG(INFO, "Ledger") << "Established RelayApplication genesis ledger";
        advanceLedgerPointers(genesisLedger);
    }
    virtual void
    loadLastKnownLedger(
        std::function<void(asio::error_code const& ec)> handler) override
    {
        handler(asio::error_code());
    };
    virtual void
    startCatchup(CatchupConfiguration configuration) override
    {
    }
    virtual void closeLedger(LedgerCloseData const& ledgerData) override{};
    virtual void deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                  uint32_t count) override{};
    virtual ~LedgerManagerStub()
    {
    }
};

class DatabaseStub : public DatabaseImpl
{
  public:
    DatabaseStub(Application& app) : DatabaseImpl(app)
    {
    }
    void
    ensureOpen() override
    {
        throw StubError();
    }
};

class CommandHandlerStub : public CommandHandlerImpl
{
  public:
    CommandHandlerStub(Application& app) : CommandHandlerImpl(app)
    {
    }
    void
    addRoutes() override
    {
        addRoute("bans", &CommandHandlerImpl::bans);
        addRoute("clearmetrics", &CommandHandlerImpl::clearMetrics);
        addRoute("connect", &CommandHandlerImpl::connect);
        addRoute("droppeer", &CommandHandlerImpl::dropPeer);
        addRoute("info", &CommandHandlerImpl::info);
        addRoute("ll", &CommandHandlerImpl::ll);
        addRoute("logrotate", &CommandHandlerImpl::logRotate);
        addRoute("manualclose", &CommandHandlerImpl::manualClose);
        addRoute("metrics", &CommandHandlerImpl::metrics);
        addRoute("peers", &CommandHandlerImpl::peers);
        addRoute("quorum", &CommandHandlerImpl::quorum);
        addRoute("scp", &CommandHandlerImpl::scpInfo);
        addRoute("unban", &CommandHandlerImpl::unban);
        addRoute("upgrades", &CommandHandlerImpl::upgrades);
    }
};

class BanManagerStub : public BanManager
{
  public:
    BanManagerStub()
    {
    }
    void
    banNode(NodeID nodeID) override
    {
    }
    void
    unbanNode(NodeID nodeID) override
    {
    }
    bool
    isBanned(NodeID nodeID) override
    {
        return false;
    }
    std::vector<std::string>
    getBans() override
    {
        return {};
    }
};

class PeerManagerStub : public PeerManagerImpl
{
    std::map<PeerBareAddress, PeerRecord> mStorage;

  public:
    PeerManagerStub(Application& app) : PeerManagerImpl(app)
    {
    }

    std::pair<PeerRecord, bool>
    load(PeerBareAddress const& address) override
    {
        std::pair<PeerRecord, bool> tmp;
        auto i = mStorage.find(address);
        if (i == mStorage.end())
        {
            tmp.first.mNextAttempt =
                VirtualClock::pointToTm(mApp.getClock().now());
            tmp.first.mType = static_cast<int>(PeerType::INBOUND);
            tmp.second = false;
        }
        else
        {
            tmp.first = i->second;
            tmp.second = true;
        }
        return tmp;
    }

    void
    store(PeerBareAddress const& address, PeerRecord const& peerRecord,
          bool inDatabase) override
    {
        mStorage[address] = peerRecord;
    }

    std::vector<PeerBareAddress>
    loadRandomPeers(PeerQuery const& query, int size) override
    {
        auto currAttempt = mApp.getClock().now();
        std::vector<PeerBareAddress> result;
        // FIXME: this is linear in the size of mStorage, which is not ideal but
        // it's in-memory and we're expecting it to be only a few hundred values
        // at most. If it becomes a problem, store a parallel randomly-indexable
        // structure.
        for (auto const& pair : mStorage)
        {
            if (query.mUseNextAttempt)
            {
                if (mApp.getClock().tmToPoint(pair.second.mNextAttempt) >
                    currAttempt)
                {
                    continue;
                }
            }
            if (pair.second.mNumFailures > query.mMaxNumFailures)
            {
                continue;
            }
            switch (query.mTypeFilter)
            {
            case PeerTypeFilter::INBOUND_ONLY:
            {
                if (pair.second.mType != static_cast<int>(PeerType::INBOUND))
                {
                    continue;
                }
                break;
            }
            case PeerTypeFilter::OUTBOUND_ONLY:
            {
                if (pair.second.mType != static_cast<int>(PeerType::OUTBOUND))
                {
                    continue;
                }
                break;
            }
            case PeerTypeFilter::PREFERRED_ONLY:
            {
                if (pair.second.mType != static_cast<int>(PeerType::PREFERRED))
                {
                    continue;
                }
                break;
            }
            case PeerTypeFilter::ANY_OUTBOUND:
            {
                if (pair.second.mType == static_cast<int>(PeerType::INBOUND))
                {
                    continue;
                }
                break;
            }
            default:
            {
                abort();
            }
            }
            result.emplace_back(pair.first);
        }
        std::shuffle(std::begin(result), std::end(result), gRandomEngine);
        result.resize(std::min(result.size(), static_cast<size_t>(size)));
        return result;
    }

    void
    removePeersWithManyFailures(int minNumFailures,
                                PeerBareAddress const* address) override
    {
    }
};

class HerderStub : public HerderImpl
{
  public:
    HerderStub(Application& app) : HerderImpl(app){};
    TransactionQueue::AddResult
    recvTransaction(TransactionFramePtr tx) override
    {
        return TransactionQueue::AddResult::ADD_STATUS_PENDING;
    }
    void
    persistSCPState(uint64 slot) override
    {
    }
    void
    persistUpgrades() override
    {
    }
    void
    restoreUpgrades() override
    {
    }
    bool
    checkTxSetValid(TxSetFramePtr txSet) override
    {
        return true;
    }
};

class PersistentStateStub : public PersistentState
{
    std::string
    getState(Entry stateName) override
    {
        return std::string();
    }
    void
    setState(Entry stateName, std::string const& value) override
    {
    }
    std::vector<std::string>
    getSCPStateAllSlots() override
    {
        return {};
    }
    void
    setSCPStateForSlot(uint64 slot, std::string const& value) override
    {
    }
};

class HerderPersistenceStub : public HerderPersistenceImpl
{
  public:
    HerderPersistenceStub(Application& app) : HerderPersistenceImpl(app)
    {
    }
    void
    saveSCPHistory(uint32_t seq, std::vector<SCPEnvelope> const& envs,
                   QuorumTracker::QuorumMap const& qmap) override
    {
    }
    optional<Hash>
    getNodeQuorumSet(Database& db, NodeID const& nodeID) override
    {
        return nullptr;
    }
    SCPQuorumSetPtr
    getQuorumSet(Database& db, Hash const& qSetHash) override
    {
        return nullptr;
    }
};

class OverlayManagerStub : public OverlayManagerImpl
{
  protected:
    std::unique_ptr<PeerManager>
    createPeerManager(Application& app) override
    {
        return std::make_unique<PeerManagerStub>(app);
    }

  public:
    OverlayManagerStub(Application& app) : OverlayManagerImpl(app)
    {
    }
};

class HistoryManagerStub : public HistoryManagerImpl
{
  public:
    HistoryManagerStub(Application& app) : HistoryManagerImpl(app)
    {
    }
    size_t
    publishQueueLength() const override
    {
        return 0;
    }
    bool
    maybeQueueHistoryCheckpoint() override
    {
        return false;
    }
    void
    queueCurrentHistory() override
    {
    }
    uint32_t
    getMinLedgerQueuedToPublish() override
    {
        return 0;
    }
    uint32_t
    getMaxLedgerQueuedToPublish() override
    {
        return 0;
    }
    size_t
    publishQueuedHistory() override
    {
        return 0;
    }
    std::vector<std::string>
    getMissingBucketsReferencedByPublishQueue() override
    {
        return {};
    }
    std::vector<std::string>
    getBucketsReferencedByPublishQueue() override
    {
        return {};
    }
    std::vector<HistoryArchiveState>
    getPublishQueueStates() override
    {
        return {};
    }

    void
    historyPublished(uint32_t ledgerSeq,
                     std::vector<std::string> const& originalBuckets,
                     bool success) override
    {
    }
    HistoryArchiveState
    getLastClosedHistoryArchiveState() const override
    {
        return HistoryArchiveState();
    }
    InferredQuorum
    inferQuorum(uint32_t ledgerNum) override
    {
        throw StubError();
    }
    std::string const&
    getTmpDir() override
    {
        throw StubError();
    }
    std::string
    localFilename(std::string const& basename) override
    {
        throw StubError();
    }
    uint64_t
    getPublishQueueCount() override
    {
        return 0;
    }
    uint64_t
    getPublishSuccessCount() override
    {
        return 0;
    }
    uint64_t
    getPublishFailureCount() override
    {
        return 0;
    }
};
}

namespace stellar
{

RelayApplication::RelayApplication(VirtualClock& clock, Config const& cfg)
    : ApplicationImpl(clock, cfg)
{
}

void
RelayApplication::initialize(ApplicationImpl::InitialDBMode initDBMode)
{
    CLOG(INFO, "Ledger") << "Initializing RelayApplication";
    ApplicationImpl::initialize(initDBMode);
    CLOG(INFO, "Ledger") << "Starting new ledger for RelayApplication";
    getLedgerManager().startNewLedger();
}

std::unique_ptr<LedgerManager>
RelayApplication::createLedgerManager()
{
    return std::make_unique<LedgerManagerStub>(*this);
}

std::unique_ptr<OverlayManager>
RelayApplication::createOverlayManager()
{
    auto ovm = std::make_unique<OverlayManagerStub>(*this);
    ovm->initialize();
    return ovm;
}

std::unique_ptr<HistoryArchiveManager>
RelayApplication::createHistoryArchiveManager()
{
    return nullptr;
}

std::unique_ptr<HistoryManager>
RelayApplication::createHistoryManager()
{
    return std::make_unique<HistoryManagerStub>(*this);
}

std::unique_ptr<Herder>
RelayApplication::createHerder()
{
    return std::make_unique<HerderStub>(*this);
}

std::unique_ptr<HerderPersistence>
RelayApplication::createHerderPersistence()
{
    return std::make_unique<HerderPersistenceStub>(*this);
}

std::unique_ptr<Database>
RelayApplication::createDatabase()
{
    return std::make_unique<DatabaseStub>(*this);
}

std::unique_ptr<BucketManager>
RelayApplication::createBucketManager()
{
    return nullptr;
}

std::unique_ptr<CatchupManager>
RelayApplication::createCatchupManager()
{
    return nullptr;
}

std::unique_ptr<Maintainer>
RelayApplication::createMaintainer()
{
    return nullptr;
}

std::unique_ptr<CommandHandler>
RelayApplication::createCommandHandler()
{
    auto cmd = std::make_unique<CommandHandlerStub>(*this);
    cmd->addRoutes();
    return cmd;
}

std::unique_ptr<BanManager>
RelayApplication::createBanManager()
{
    return std::make_unique<BanManagerStub>();
}

std::unique_ptr<LedgerTxnRoot>
RelayApplication::createLedgerTxnRoot()
{
    return nullptr;
}

std::shared_ptr<WorkScheduler>
RelayApplication::createWorkScheduler()
{
    return nullptr;
}

std::unique_ptr<PersistentState>
RelayApplication::createPersistentState()
{
    return std::make_unique<PersistentStateStub>();
}

std::shared_ptr<ProcessManager>
RelayApplication::createProcessManager()
{
    return nullptr;
}
}
