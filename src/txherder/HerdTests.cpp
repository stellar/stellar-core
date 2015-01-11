// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "lib/json/json.h"


using namespace stellar;

typedef std::unique_ptr<Application> appPtr;

// see if we flood at the right times
//  invalid tx
//  normal tx
//  tx with bad seq num
//  account can't pay for all the tx
//  account has just enough for all the tx
//  tx from account not in the DB
TEST_CASE("recvTx", "[txh]")
{
   

}


// sortForApply 
// sortForHash
// checkValid
//   not sorted correctly
//   tx with bad seq num
//   account can't pay for all the tx
//   account has just enough for all the tx
//   tx from account not in the DB 
TEST_CASE("txset", "[txh]")
{

}


// test isValidBallotValue