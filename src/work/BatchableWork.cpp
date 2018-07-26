#include "work/BatchableWork.h"

namespace stellar
{
BatchableWork::BatchableWork(Application& app, WorkParent& parent,
                             std::string name, size_t maxRetries)
    : Work(app, parent, fmt::format("batchable-" + name), maxRetries)
{
}

BatchableWork::~BatchableWork()
{
    clearChildren();
}

void
BatchableWork::registerDependent(std::shared_ptr<BatchableWork> blockedWork)
{
    mDependentSnapshot = blockedWork;
}

void
BatchableWork::notifyCompleted()
{
    if (mDependentSnapshot)
    {
        mDependentSnapshot->unblockWork(mSnapshotData);
    }
}

void
BatchableWork::unblockWork(BatchableWorkResultData const& data)
{
    // Do nothing: each subclass decides how to proceed
}

std::shared_ptr<BatchableWork>
BatchableWork::getDependent()
{
    return mDependentSnapshot;
}
}
