// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "generated/StellarXDR.h"
#include "history/HistoryMaster.h"
#include "util/make_unique.h"
#include "util/Logging.h"
#include "util/TempDir.h"
#include "lib/util/format.h"

#include "xdrpp/marshal.h"

#include <fstream>

namespace stellar
{

class
HistoryMaster::Impl
{
    Application& mApp;
    TempDir mWorkDir;
    friend class HistoryMaster;
public:
    Impl(Application &app)
        : mApp(app)
        , mWorkDir("history")
        {}

};


HistoryMaster::HistoryMaster(Application& app)
    : mImpl(make_unique<Impl>(app))
{
}

HistoryMaster::~HistoryMaster()
{
}


/*
 * HistoryMaster behaves differently depending on application state.
 *
 * When application is in anything before CATCHING_UP_STATE, HistoryMaster
 * should be idle.
 *
 * When application is in CATCHING_UP_STATE, HistoryMaster picks an "anchor"
 * ledger X that it's trying to build a complete bucketlist for, as well as a
 * history-suffix from [X,current] as current advances. It downloads buckets
 * for X and accumulates history entries from X forward.
 *
 * Once it has a full set of buckets for X, it will force-apply them to the
 * database (overwrite objects) in order to acquire state X. If successful
 * it will then play transactions forward from X, as fast as it can, until
 * X==current. At that point it will switch to SYNCED_STATE.
 *
 * in SYNCED_STATE it accumulates history entries as they arrive, in the
 * database, periodically flushes those to a file on disk, copies it to s3 (or
 * wherever) and deletes the archived entries. It also flushes buckets to
 * s3 as they are evicted from the CLF.
 */

std::string
HistoryMaster::writeLedgerHistoryToFile(History const& hist)
{
    std::string fname = fmt::format("{:s}/history_{:d}_{:d}.xdr",
                                    mImpl->mWorkDir.getName(),
                                    hist.fromLedger,
                                    hist.toLedger);
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
