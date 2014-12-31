#ifndef __STATEMENT__
#define __STATEMENT__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "txherder/TxHerderGateway.h"
#include "fba/FBA.h"

namespace stellar
{
class Application;

class Statement
{
  protected:
    TxHerderGateway::BallotValidType mValidity;

    virtual void
    fillXDRBody(FBAContents& body)
    {
    }

  public:
    typedef std::shared_ptr<Statement> pointer;

    FBAEnvelope mEnvelope;
    uint256 mContentsHash; // TODO.1 should calculate this somewhere

    Statement();
    // creates a Statement from the wire
    Statement(FBAEnvelope const& envelope);
    Statement(FBAStatementType type,
              uint256 const& nodeID,
              uint256 const& qSetHash,
              const SlotBallot& ballot);

    void sign();

    FBAStatementType
    getType()
    {
        return mEnvelope.contents.body.type();
    }

    SlotBallot&
    getSlotBallot()
    {
        return mEnvelope.contents.slotBallot;
    }
    Ballot&
    getBallot()
    {
        return mEnvelope.contents.slotBallot.ballot;
    }

    bool operator==(Statement const& other)
    {
        return mContentsHash == other.mContentsHash;
    }

    bool isCompatible(BallotPtr ballot);

    TxHerderGateway::BallotValidType checkValidity(Application& app);

    bool compare(Statement::pointer other);

    // checks signature
    bool isSigValid();

    // check that it includes any tx you think are mandatory
    bool isTxSetValid();

    uint32_t getLedgerIndex();

    TxSetFramePtr fetchTxSet(Application& app);
};
}

#endif
