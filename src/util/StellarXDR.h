#pragma once

#ifndef _WIN32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-overlay.h"
#include "xdr/Stellar-transaction.h"
#include "xdr/Stellar-types.h"

#ifndef _WIN32
#pragma GCC diagnostic pop
#endif

namespace stellar
{

std::string xdr_printer(const PublicKey& pk);
}
