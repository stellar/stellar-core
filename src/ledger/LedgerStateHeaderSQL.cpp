// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "xdrpp/marshal.h"
#include <util/basen.h>

namespace stellar
{

void
LedgerState::storeHeaderInDatabase()
{
    assert(mRoot);

    // TODO(jonjove): Check if LedgerHeader is valid

    auto const& header = mHeader->ignoreInvalid().header();
    auto headerBytes(xdr::xdr_to_opaque(header));
    std::string hash(binToHex(sha256(headerBytes))),
        prevHash(binToHex(header.previousLedgerHash)),
        bucketListHash(binToHex(header.bucketListHash));

    std::string headerEncoded;
    headerEncoded = bn::encode_b64(headerBytes);

    auto& db = mRoot->getDatabase();

    // note: columns other than "data" are there to faciliate lookup/processing
    auto prep = db.getPreparedStatement(
        "INSERT INTO ledgerheaders "
        "(ledgerhash, prevhash, bucketlisthash, ledgerseq, closetime, data) "
        "VALUES "
        "(:h,        :ph,      :blh,            :seq,     :ct,       :data)");
    auto& st = prep.statement();
    st.exchange(soci::use(hash));
    st.exchange(soci::use(prevHash));
    st.exchange(soci::use(bucketListHash));
    st.exchange(soci::use(header.ledgerSeq));
    st.exchange(soci::use(header.scpValue.closeTime));
    st.exchange(soci::use(headerEncoded));
    st.define_and_bind();
    {
        auto timer = db.getInsertTimer("ledger-header");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}
}
