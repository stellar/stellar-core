#pragma once
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-overlay.h"
#include "xdr/Stellar-transaction.h"
#include "xdr/Stellar-types.h"

namespace stellar
{

std::string xdr_printer(PublicKey const& pk);
std::string xdr_printer(MuxedAccount const& ma);
}
