#include "historywork/BestEffortBatchDownloadWork.h"

namespace stellar
{
BestEffortBatchDownloadWork::BestEffortBatchDownloadWork(
    Application& app, CheckpointRange range, std::string const& type,
    std::shared_ptr<TmpDir const> downloadDir,
    std::shared_ptr<HistoryArchive> archive)
    : BatchDownloadWork(app, range, type, downloadDir, archive)
{
}

BasicWork::State
BestEffortBatchDownloadWork::onChildFailure()
{
    // TODO: Emit a warning about the failed download
    abortSuccess();
    return State::WORK_RUNNING;
}
} // namespace stellar