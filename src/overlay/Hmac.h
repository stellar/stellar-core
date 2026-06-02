// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "Tracy.hpp"
#include "util/ThreadAnnotations.h"
#include "xdr/Stellar-overlay.h"
#include "xdr/Stellar-types.h"

using namespace stellar;

namespace stellar
{
class Peer;
}

class Hmac
{
#ifdef THREAD_SAFETY
    // Make the peer class a friend for thread safety analysis
    friend class stellar::Peer;
#endif

    ANNOTATED_MUTEX(mMutex);
    HmacSha256Key mSendMacKey GUARDED_BY(mMutex);
    HmacSha256Key mRecvMacKey GUARDED_BY(mMutex);
    uint64_t mSendMacSeq GUARDED_BY(mMutex){0};
    uint64_t mRecvMacSeq GUARDED_BY(mMutex){0};

  public:
    bool setSendMackey(HmacSha256Key const& key);
    bool setRecvMackey(HmacSha256Key const& key);
    bool checkAuthenticatedMessage(AuthenticatedMessage const& msg,
                                   std::string& errorMsg);
    void setAuthenticatedMessageBody(AuthenticatedMessage& aMsg,
                                     StellarMessage const& msg);
#ifdef BUILD_TESTS
    void damageRecvMacKey();
#endif
};
