#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "transactions/TransactionMeta.h"

namespace stellar
{
class AppConnector;

// This class is used during parallel transaction application to manage the
// state and effects of each individual transaction as it is validated and
// applied to the ledger. It serves as a container that keeps all
// transaction-related components together as a single unit.

class TxEffects
{
  public:
    TxEffects(bool enableTxMeta, TransactionFrameBase const& tx,
              uint32_t ledgerVersion, AppConnector const& app)
        : mMeta(enableTxMeta, tx, ledgerVersion, app)
    {
    }

    TransactionMetaBuilder&
    getMeta()
    {
        return mMeta;
    }
    LedgerTxnDelta const&
    getDelta()
    {
        return mDelta;
    }

    void
    setDeltaEntry(LedgerKey const& key, LedgerTxnDelta::EntryDelta const& delta)
    {
        auto [_, inserted] = mDelta.entry.emplace(key, delta);
        releaseAssertOrThrow(inserted);
    }

    void
    setDeltaHeader(LedgerHeader const& header)
    {
        mDelta.header.current = header;
        mDelta.header.previous = header;
    }

  private:
    TransactionMetaBuilder mMeta;
    LedgerTxnDelta mDelta;
};

// TxBundle contains a transaction, its associated result payload, and its
// effects. It provides a convenient way to group and manage transaction-related
// data during processing and application to the ledger.
class TxBundle
{
  public:
    TxBundle(AppConnector const& app, TransactionFrameBasePtr tx,
             MutableTransactionResultBase& resPayload, uint32_t ledgerVersion,
             uint64_t txNum, bool enableTxMeta)
        : mTx(tx)
        , mResPayload(resPayload)
        , mTxNum(txNum)
        , mEffects(new TxEffects(enableTxMeta, *tx, ledgerVersion, app))
    {
    }

    TransactionFrameBasePtr
    getTx() const
    {
        return mTx;
    }
    MutableTransactionResultBase&
    getResPayload() const
    {
        return mResPayload;
    }
    uint64_t
    getTxNum() const
    {
        return mTxNum;
    }
    TxEffects&
    getEffects() const
    {
        return *mEffects;
    }

  private:
    TransactionFrameBasePtr mTx;
    // Holding a reference to the results is somewhat of a footgun
    MutableTransactionResultBase& mResPayload;
    uint64_t mTxNum;
    std::unique_ptr<TxEffects> mEffects;
};

typedef std::vector<TxBundle> Cluster;

class ApplyStage
{
  public:
    ApplyStage(std::vector<Cluster>&& clusters) : mClusters(std::move(clusters))
    {
    }

    class Iterator
    {
      public:
        using value_type = TxBundle;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;
        using iterator_category = std::forward_iterator_tag;

        TxBundle const& operator*() const;

        Iterator& operator++();
        Iterator operator++(int);

        bool operator==(Iterator const& other) const;
        bool operator!=(Iterator const& other) const;

      private:
        friend class ApplyStage;

        Iterator(std::vector<Cluster> const& clusters, size_t clusterIndex);
        std::vector<Cluster> const& mClusters;
        size_t mClusterIndex = 0;
        size_t mTxIndex = 0;
    };
    Iterator begin() const;
    Iterator end() const;

    Cluster const& getCluster(size_t i) const;
    size_t numClusters() const;

  private:
    std::vector<Cluster> mClusters;
};

}