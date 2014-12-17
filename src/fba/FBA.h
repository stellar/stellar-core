#ifndef __FBA__
#define __FBA__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

namespace stellar
{
typedef std::shared_ptr<stellarxdr::Ballot> BallotPtr;
typedef std::shared_ptr<stellarxdr::SlotBallot> SlotBallotPtr;

class Statement;
typedef std::shared_ptr<Statement> StatementPtr;

class FutureStatement;
typedef std::shared_ptr<FutureStatement> FutureStatementPtr;
}

// beyond this then the ballot is considered invalid
#define MAX_SECONDS_LEDGER_CLOSE_IN_FUTURE 2

// how far in the future to guess the ledger will close
#define NUM_SECONDS_IN_CLOSE 2

#endif
