#include "overlay/Hmac.h"
#ifdef BUILD_TESTS
#include "crypto/Random.h"
#endif
#include "crypto/SHA.h"
#include "util/GlobalChecks.h"
#include "util/types.h"
#include <xdrpp/marshal.h>

bool
Hmac::setSendMackey(HmacSha256Key const& key)
{
    ZoneScoped;
    LOCK_GUARD(mMutex, guard);
    if (!isZero(mSendMacKey.key))
    {
        return false;
    }
    mSendMacKey = key;
    return true;
}

bool
Hmac::setRecvMackey(HmacSha256Key const& key)
{
    ZoneScoped;
    LOCK_GUARD(mMutex, guard);
    if (!isZero(mRecvMacKey.key))
    {
        return false;
    }
    mRecvMacKey = key;
    return true;
}

bool
Hmac::checkAuthenticatedMessage(AuthenticatedMessage const& msg,
                                std::string& errorMsg)
{
    ZoneScoped;
    LOCK_GUARD(mMutex, guard);

    if (msg.v0().sequence != mRecvMacSeq)
    {
        errorMsg = "unexpected auth sequence";
        return false;
    }
    if (isZero(mRecvMacKey.key))
    {
        errorMsg = "receive mac key is zero";
        return false;
    }
    if (!hmacSha256Verify(
            msg.v0().mac, mRecvMacKey,
            xdr::xdr_to_opaque(msg.v0().sequence, msg.v0().message)))
    {
        errorMsg = "unexpected MAC";
        return false;
    }
    ++mRecvMacSeq;
    return true;
}

void
Hmac::setAuthenticatedMessageBody(AuthenticatedMessage& aMsg,
                                  StellarMessage const& msg)

{
    ZoneScoped;
    LOCK_GUARD(mMutex, guard);

    aMsg.v0().message = msg;
    if (msg.type() != HELLO && msg.type() != ERROR_MSG)
    {
        aMsg.v0().sequence = mSendMacSeq;
        aMsg.v0().mac =
            hmacSha256(mSendMacKey, xdr::xdr_to_opaque(mSendMacSeq, msg));
        mSendMacSeq++;
    }
}

#ifdef BUILD_TESTS
void
Hmac::damageRecvMacKey()
{
    auto bytes = randomBytes(mRecvMacKey.key.size());
    std::copy(bytes.begin(), bytes.end(), mRecvMacKey.key.begin());
}
#endif