// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "generated/StellarXDR.h"
#include "history/HistoryMaster.h"
#include "util/Logging.h"
#include "lib/util/format.h"

#include "xdrpp/marshal.h"

#include <fstream>

namespace stellar
{

HistoryMaster::HistoryMaster(Application& app) : mApp(app)
{
}

std::string
HistoryMaster::writeLedgerHistoryToFile(History const& hist)
{
    std::string fname = fmt::format("history_{:d}_{:d}.xdr",
                                    hist.fromLedger, hist.toLedger);
    LOG(INFO) << "Writing history block " << fname;
    std::ofstream out(fname, std::ofstream::binary);
    auto m = xdr::xdr_to_msg(hist);
    out.write(m->raw_data(), m->raw_size());
    return fname;
}

void
HistoryMaster::readLedgerHistoryFromFile(std::string const& fname,
                                         History& hist)
{
    LOG(INFO) << "Reading history block " << fname;
    std::ifstream in(fname, std::ofstream::binary);
    in.exceptions(std::ifstream::failbit);
    in.seekg(0, in.end);
    std::ifstream::pos_type length = in.tellg();
    in.seekg(0, in.beg);
    xdr::msg_ptr m = xdr::message_t::alloc(length-4LL);
    in.read(m->raw_data(), m->raw_size());
    in.close();
    xdr::xdr_from_msg(m, hist);
}

}
