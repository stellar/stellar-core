
#include "herder/Herder.h"

namespace stellar
{

std::chrono::milliseconds const
    Herder::TARGET_LEDGER_CLOSE_TIME_BEFORE_PROTOCOL_VERSION_23_MS(5000);
std::chrono::seconds const Herder::MAX_SCP_TIMEOUT_SECONDS(240);
std::chrono::seconds const Herder::CONSENSUS_STUCK_TIMEOUT_SECONDS(35);
std::chrono::seconds const Herder::OUT_OF_SYNC_RECOVERY_TIMER =
    std::chrono::seconds(10);
std::chrono::seconds const Herder::SEND_LATEST_CHECKPOINT_DELAY =
    std::chrono::seconds(2);
std::chrono::seconds constexpr Herder::MAX_TIME_SLIP_SECONDS;
std::chrono::seconds const Herder::NODE_EXPIRATION_SECONDS(240);
// the value of LEDGER_VALIDITY_BRACKET should be in the order of
// how many ledgers can close ahead given CONSENSUS_STUCK_TIMEOUT_SECONDS
uint32 const Herder::LEDGER_VALIDITY_BRACKET = 100;
std::chrono::nanoseconds const Herder::TIMERS_THRESHOLD_NANOSEC(5000000);
uint32 const Herder::SCP_EXTRA_LOOKBACK_LEDGERS = 3u;
std::chrono::minutes const Herder::TX_SET_GC_DELAY(1);
std::chrono::minutes const Herder::CHECK_FOR_DEAD_NODES_MINUTES(15);
uint32 const Herder::FLOW_CONTROL_BYTES_EXTRA_BUFFER(2000);

Hash const Herder::SKIP_LEDGER_HASH{};
}
