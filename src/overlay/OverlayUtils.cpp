#include "overlay/OverlayUtils.h"
#include "main/ErrorMessages.h"
#include "util/Logging.h"

namespace stellar
{

void
logErrorOrThrow(std::string const& message)
{
    CLOG_ERROR(Overlay, "{}", message);
#ifdef BUILD_TESTS
    throw std::runtime_error(message);
#else
    CLOG_ERROR(Overlay, REPORT_INTERNAL_BUG);
#endif
}
}
