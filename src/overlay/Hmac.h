#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Tracy.hpp"
#include "xdr/Stellar-overlay.h"
#include "xdr/Stellar-types.h"
#include <mutex>

using namespace stellar;

class Hmac
{
#ifndef USE_TRACY
    std::mutex mMutex;
#else
    TracyLockable(std::mutex, mMutex);
#endif
    HmacSha256Key mSendMacKey;
    HmacSha256Key mRecvMacKey;
    uint64_t mSendMacSeq{0};
    uint64_t mRecvMacSeq{0};

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