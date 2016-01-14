#include "util/XDRStream.h"
#include "util/Fs.h"
#include "main/dumpxdr.h"
#include <xdrpp/printer.h>
#include <regex>
#include <iostream>

namespace stellar
{

template <typename T>
void
dumpstream(XDRInputFileStream& in)
{
    T tmp;
    while (in && in.readOne(tmp))
    {
        std::cout << xdr::xdr_to_string(tmp) << std::endl;
    }
}

void
dumpxdr(std::string const& filename)
{
    std::regex rx(".*(ledger|bucket|transactions|results|scp)-[[:xdigit:]]+\\.xdr");
    std::smatch sm;
    if (std::regex_match(filename, sm, rx))
    {
        XDRInputFileStream in;
        in.open(filename);

        if (sm[1] == "ledger")
        {
            dumpstream<LedgerHeaderHistoryEntry>(in);
        }
        else if (sm[1] == "bucket")
        {
            dumpstream<BucketEntry>(in);
        }
        else if (sm[1] == "transactions")
        {
            dumpstream<TransactionHistoryEntry>(in);
        }
        else if (sm[1] == "results")
        {
            dumpstream<TransactionHistoryResultEntry>(in);
        }
        else
        {
            assert(sm[1] == "scp");
            dumpstream<SCPHistoryEntry>(in);
        }
    }
    else
    {
        throw std::runtime_error("unrecognized XDR filename");
    }
}
}
