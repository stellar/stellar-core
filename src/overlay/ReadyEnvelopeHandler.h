#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{

class ReadyEnvelopeHandler
{
  public:
    virtual ~ReadyEnvelopeHandler()
    {
    }

    /**
     * Called when new ready (fully-fetched) envelope is available in
     * PendingEnvelopes.
     */
    virtual void readyEnvelopeAvailable() = 0;
};
}
