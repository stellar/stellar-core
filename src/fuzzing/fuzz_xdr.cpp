#include "xdr/Stellar-ledger-entries.h"
#include <cstdint>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    try {
        stellar::LedgerEntry entry;
        xdr::xdr_from_opaque(data, entry); // ohne size, wenn data ein std::vector<uint8_t> ist
    } catch (...) {
        // Ignoriere Parsing-Fehler
    }
    return 0;
}
