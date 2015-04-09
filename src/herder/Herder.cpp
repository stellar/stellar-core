
#include "herder/Herder.h"

namespace stellar
{

std::chrono::seconds const Herder::EXP_LEDGER_TIMESPAN_SECONDS(5);
std::chrono::seconds const Herder::MAX_SCP_TIMEOUT_SECONDS(240);
std::chrono::seconds const Herder::MAX_TIME_SLIP_SECONDS(60);
std::chrono::seconds const Herder::NODE_EXPIRATION_SECONDS(240);
uint32 const Herder::LEDGER_VALIDITY_BRACKET = 1000;


}
