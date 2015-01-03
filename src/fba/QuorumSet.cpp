// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "QuorumSet.h"
#include "main/Application.h"
#include "xdrpp/marshal.h"
#include "lib/util/easylogging++.h"
#include "crypto/SHA.h"
/*
Need to ensure that threshold is > 50% of the nodes or the network won't be
confluent

*/

namespace stellar
{
QuorumSet::QuorumSet()
{
    mThreshold = 0;
}

// get qset from wire
QuorumSet::QuorumSet(QuorumSetDesc const& qset, Application& app)
{
    mThreshold = qset.threshold;
    for (auto id : qset.validators)
    {
        mNodes.push_back(app.getFBAGateway().getNode(id));
    }
}

uint256
QuorumSet::getHash()
{
    if (isZero(mHash))
    {
        QuorumSetDesc qset;
        toXDR(qset);
        mHash = sha512_256(xdr::xdr_to_msg(qset));
    }
    return mHash;
}

size_t
QuorumSet::getBlockingSize()
{
    return 1 + mNodes.size() - mThreshold;
}

// returns true if the first argument is ordered before the second.
bool
ballotSorter(const BallotSet& a, const BallotSet& b)
{
    return ballot::compare(a.mBallot.ballot, b.mBallot.ballot);
}

// returns all the ballots sorted by rank
void
QuorumSet::sortBallots(FBAStatementType type,
                       vector<BallotSet>& retList)
{
    for (auto node : mNodes)
    {
        Statement::pointer statement = node->getHighestStatement(type);
        if (statement)
        {
            bool found = false;
            // check if this ballot is already on the list
            // check if any of the ballots are compatible with this one
            for (auto cballot : retList)
            {
                if (ballot::isCompatible(cballot.mBallot.ballot,
                                         statement->getBallot()))
                {
                    cballot.mCount++;
                    found = true;
                }
            }
            if (!found)
                retList.push_back(BallotSet(statement->getSlotBallot()));
        }
    }
    // sort the list
    sort(retList.begin(), retList.end(), ballotSorter);
}

BallotPtr
QuorumSet::getMostPopularBallot(FBAStatementType type,
                                bool checkValid, Application& app)
{
    /* TODO.1 wait for xdrpp to include a comparison
            map< Ballot, int> ballotCounts;


            for(auto node : mNodes)
            {
                    Statement::pointer statement =
    node->getHighestStatement(type);
                    if(statement)
                    {
                            if(!checkValid ||
    app.getTxHerderGateway().isValidBallotValue(statement->getBallot()))
                            {
                Ballot ballot = statement->getBallot();
                ballot.index = 0;
                                    ballotCounts[ballot] += 1;
                            }
                    }
            }
    Ballot mostPopular;
            int mostPopularCount = 0;
    bool foundOne = false;
            for(auto bcount : ballotCounts)
            {
                    if(bcount.second > mostPopularCount)
                    {
            foundOne = true;
                            mostPopular = bcount.first;
                            mostPopularCount = bcount.second;
                    }
            }

            if(foundOne)
    {
                    return std::make_shared<Ballot>(mostPopular);
            }
            */
    return BallotPtr();
}

// get the highest valid statement
Statement::pointer
QuorumSet::getHighestStatement(FBAStatementType type,
                               bool checkValid, Application& app)
{
    Statement::pointer highStatement;
    for (auto node : mNodes)
    {
        Statement::pointer statement = node->getHighestStatement(type);
        if (!checkValid ||
            app.getTxHerderGateway().isValidBallotValue(statement->getBallot()))
        {
            if (!highStatement)
                highStatement = statement;
            else
            {
                if (statement && statement->compare(highStatement))
                {
                    highStatement = statement;
                }
            }
        }
    }

    return highStatement;
}

// loop through the nodes and see if
// a) they are pledging a compatible ballot
// b) they have ratified
// for PREPARE we need to look at gaps
//		for any gap see if other people can ratify the abort
Node::RatState
QuorumSet::checkRatState(FBAStatementType statementType,
                         BallotPtr ballot, int operationToken,
                         int recheckCounter, Application& app)
{
    // LATER if(statementType == Statement::PREPARE_TYPE) return
    // checkPrepareRatState(statement, visitIndex);

    int ratCount = 0;
    for (auto node : mNodes)
    {
        Node::RatState state = node->checkRatState(
            statementType, ballot, operationToken, recheckCounter, app);
        if (state == Node::PLEDGING_STATE || state == Node::RATIFIED_STATE)
        {
            ratCount++;
            if (ratCount > mThreshold)
                return Node::PLEDGING_STATE;
        }
    }
    return Node::Q_NOT_PLEDGING_STATE;
}

// like checkRatState except we must take exceptions into account
Node::RatState
QuorumSet::checkPrepareRatState(Statement::pointer statement, int visitIndex)
{
    // LATER
    return Node::PLEDGING_STATE;
}

void
QuorumSet::toXDR(QuorumSetDesc& qSet)
{
    qSet.threshold = mThreshold;
    for (auto val : mNodes)
    {
        qSet.validators.push_back(val->mNodeID);
    }
}
}
