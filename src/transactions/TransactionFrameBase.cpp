// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionFrameBase.h"
#include "transactions/FeeBumpTransactionFrame.h"
#include "transactions/TransactionFrame.h"

namespace stellar
{

TransactionFrameBasePtr
TransactionFrameBase::makeTransactionFromWire(Hash const& networkID,
                                              TransactionEnvelope const& env)
{
    switch (env.type())
    {
    case ENVELOPE_TYPE_TX_V0:
    case ENVELOPE_TYPE_TX:
        return std::make_shared<TransactionFrame>(networkID, env);
    case ENVELOPE_TYPE_TX_FEE_BUMP:
        return std::make_shared<FeeBumpTransactionFrame>(networkID, env);
    default:
        abort();
    }
}

ApplyStage::Iterator::Iterator(std::vector<Cluster> const& clusters,
                               size_t clusterIndex)
    : mClusters(clusters), mClusterIndex(clusterIndex)
{
}

TxBundle const&
ApplyStage::Iterator::operator*() const
{

    if (mClusterIndex >= mClusters.size() ||
        mTxIndex >= mClusters[mClusterIndex].size())
    {
        throw std::runtime_error("TxPhase iterator out of bounds");
    }
    return mClusters[mClusterIndex][mTxIndex];
}

ApplyStage::Iterator&
ApplyStage::Iterator::operator++()
{
    if (mClusterIndex >= mClusters.size())
    {
        throw std::runtime_error("TxPhase iterator out of bounds");
    }
    ++mTxIndex;
    if (mTxIndex >= mClusters[mClusterIndex].size())
    {
        mTxIndex = 0;
        ++mClusterIndex;
    }
    return *this;
}

ApplyStage::Iterator
ApplyStage::Iterator::operator++(int)
{
    auto it = *this;
    ++(*this);
    return it;
}

bool
ApplyStage::Iterator::operator==(Iterator const& other) const
{
    return mClusterIndex == other.mClusterIndex && mTxIndex == other.mTxIndex &&
           &mClusters == &other.mClusters;
}

bool
ApplyStage::Iterator::operator!=(Iterator const& other) const
{
    return !(*this == other);
}

ApplyStage::Iterator
ApplyStage::begin() const
{
    return ApplyStage::Iterator(mClusters, 0);
}

ApplyStage::Iterator
ApplyStage::end() const
{
    return ApplyStage::Iterator(mClusters, mClusters.size());
}

Cluster const&
ApplyStage::getCluster(size_t i) const
{
    return mClusters.at(i);
}

size_t
ApplyStage::numClusters() const
{
    return mClusters.size();
}

}