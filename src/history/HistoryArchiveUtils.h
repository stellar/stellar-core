#pragma once

#include "util/XDRStream.h"
#include <functional>
#include <type_traits>

namespace stellar
{

// Type trait to check for ledgerSeq member.
template <typename T, typename = void> struct has_ledger_seq : std::false_type
{
};
template <typename T>
struct has_ledger_seq<T, std::void_t<decltype(std::declval<T>().ledgerSeq)>>
    : std::true_type
{
};

class HistoryArchiveUtils
{
  public:
    template <typename T> using ValidationFunc = std::function<bool(T const&)>;

    // Read next entry from XDRInputFileStream at targetLedger.
    // Returns true if an entry matching targetLedger was found,
    // false otherwise.
    // Callers must ensure that the stream is open, and reuse the same
    // entry object for each call.
    // ValidationFunc<T>, if provided, is called on each entry
    // to validate it immediately after reading it from the stream.
    template <typename T, typename = std::enable_if_t<has_ledger_seq<T>::value>>
    static bool
    readNextEntry(
        T& entry, XDRInputFileStream& in, uint32_t targetLedger,
        std::optional<ValidationFunc<T>> validationFunc = std::nullopt)
    {

        do
        {
            if (entry.ledgerSeq < targetLedger)
            {
                CLOG_DEBUG(History,
                           "Advancing past ledger {} before target ledger {}",
                           entry.ledgerSeq, targetLedger);
                // Skip entries before targetLedger.
                continue;
            }
            else if (entry.ledgerSeq > targetLedger)
            {
                // No entry for this ledger.
                break;
            }
            else
            {
                // Found matching entry.
                return true;
            }
        } while (in && in.readOne(entry) &&
                 (validationFunc ? (*validationFunc)(entry) : true));

        return false;
    }
};

} // namespace stellar