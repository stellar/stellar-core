// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/FuzzerImpl.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "overlay/TCPPeer.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/fuzz.h"
#include "test/test.h"
#include "transactions/OperationFrame.h"
#include "transactions/SignatureChecker.h"
#include "util/Math.h"

#include <xdrpp/autocheck.h>

namespace stellar
{
namespace FuzzUtils
{
auto const FUZZER_MAX_OPERATIONS = 5;
auto const INITIAL_LUMEN_AND_ASSET_BALANCE = 100000LL;
auto const NUMBER_OF_ASSETS_TO_ISSUE = 4;

// generates a public key such that it's comprised of bytes reading [0,0,...,i]
PublicKey
makePublicKey(int i)
{
    PublicKey publicKey;
    uint256 accountID;
    accountID.at(31) = i;
    publicKey.ed25519() = accountID;
    return publicKey;
}

// constructs an Asset structure for an asset issued by an account id comprised
// of bytes reading [0,0,...,i] and an alphanum4 asset code of "Ast + i"
Asset
makeAsset(int i)
{
    Asset asset;
    asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(asset.alphaNum4().assetCode, "Ast" + std::to_string(i));
    asset.alphaNum4().issuer = makePublicKey(i);
    return asset;
}
}

// creates a generic configuration with settings rigged to maximize
// determinism
static Config
getFuzzConfig(int instanceNumber)
{
    Config cfg = getTestConfig(instanceNumber);
    cfg.MANUAL_CLOSE = true;
    cfg.CATCHUP_COMPLETE = false;
    cfg.CATCHUP_RECENT = 0;
    cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = false;
    cfg.ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING = UINT32_MAX;
    cfg.PUBLIC_HTTP_PORT = false;
    cfg.WORKER_THREADS = 1;
    cfg.QUORUM_INTERSECTION_CHECKER = false;
    cfg.PREFERRED_PEERS_ONLY = false;
    cfg.RUN_STANDALONE = true;

    return cfg;
}

void
resetTxInternalState(Application& app)
{
    // seed randomness
    srand(1);
    gRandomEngine.seed(1);

// reset caches to clear persistent state
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    app.getLedgerTxnRoot().resetForFuzzer();
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    app.getDatabase().clearPreparedStatementCache();
}

// FuzzTransactionFrame is a specialized TransactionFrame that includes
// useful methods for fuzzing such as an attemptApplication method for reseting
// ledger state and deterministically attempting application of transactions.
class FuzzTransactionFrame : public TransactionFrame
{
  public:
    FuzzTransactionFrame(Hash const& networkID,
                         TransactionEnvelope const& envelope)
        : TransactionFrame(networkID, envelope){};

    void
    attemptApplication(Application& app, AbstractLedgerTxn& ltx)
    {
        // reset results of operations
        resetResults(ltx.getHeader(), 0);

        // attempt application of transaction without accounting for sequence
        // number, processing the fee, or committing the LedgerTxn
        SignatureChecker signatureChecker{
            ltx.loadHeader().current().ledgerVersion, getContentsHash(),
            mEnvelope.signatures};
        // if any ill-formed Operations, do not attempt transaction application
        auto isInvalidOperationXDR = [&](auto const& op) {
            return !op->checkValid(signatureChecker, ltx, false);
        };
        if (std::any_of(mOperations.begin(), mOperations.end(),
                        isInvalidOperationXDR))
        {
            return;
        }
        // while the following method's result is not captured, regardless, for
        // protocols < 8, this triggered buggy caching, and potentially may do
        // so in the future
        loadSourceAccount(ltx, ltx.loadHeader());
        TransactionMeta tm(1);
        applyOperations(signatureChecker, app, ltx, tm.v1());
    }
};

std::shared_ptr<FuzzTransactionFrame>
createFuzzTransactionFrame(PublicKey sourceAccountID,
                           std::vector<Operation> ops, Hash const& networkID)
{
    // construct a transaction envelope, which, for each transaction
    // application in the fuzzer, is the exact same, except for the inner
    // operations of course
    auto txEnv = TransactionEnvelope{};
    txEnv.tx.sourceAccount = sourceAccountID;
    txEnv.tx.fee = 0;
    txEnv.tx.seqNum = 1;
    std::copy(std::begin(ops), std::end(ops),
              std::back_inserter(txEnv.tx.operations));

    std::shared_ptr<FuzzTransactionFrame> res =
        std::make_shared<FuzzTransactionFrame>(networkID, txEnv);
    return res;
}

bool
isBadOverlayFuzzerInput(StellarMessage const& m)
{
    // HELLO, AUTH and ERROR_MSG messages cause the connection between
    // the peers to drop. Since peer connections are only established
    // preceding the persistent loop, a dropped peer is not only
    // inconvenient, it also confuses the fuzzer. Consider a msg A sent
    // before a peer is dropped and after a peer is dropped. The two,
    // even though the same message, will take drastically different
    // execution paths -- the fuzzer's main metric for determinism
    // (stability) and binary coverage.
    return m.type() == AUTH || m.type() == ERROR_MSG || m.type() == HELLO;
}

void
TransactionFuzzer::initialize()
{
    VirtualClock clock;
    mApp = createTestApplication(clock, getFuzzConfig(mProcessID));

    resetTxInternalState(*mApp);
    LedgerTxn ltxroot(mApp->getLedgerTxnRoot());

    {
        LedgerTxn ltx(ltxroot);

        // Setup the state, for this we only need to pregenerate some
        // accounts. For now we create mNumAccounts accounts, or enough to
        // fill the first few bits such that we have a pregenerated account
        // for the last few bits of the 32nd byte of a public key, thus
        // account creation is over a deterministic range of public keys
        for (int i = 0; i < mNumAccounts; ++i)
        {
            PublicKey publicKey = FuzzUtils::makePublicKey(i);

            // manually construct ledger entries, "creating" each account
            LedgerEntry newAccountEntry;
            newAccountEntry.data.type(ACCOUNT);
            auto& newAccount = newAccountEntry.data.account();
            newAccount.thresholds[0] = 1;
            newAccount.accountID = publicKey;
            newAccount.seqNum = 0;
            // convert lumens to stroops; required by low level ledger entry
            // structure which operates in stroops
            newAccount.balance =
                FuzzUtils::INITIAL_LUMEN_AND_ASSET_BALANCE * 10000000;

            ltx.create(newAccountEntry);

            // select the first pregenerated account to be the hard coded source
            // account for all transactions
            if (i == 0)
            {
                mSourceAccountID = publicKey;
            }
        }

        ltx.commit();
    }

    {
        LedgerTxn ltx(ltxroot);

        xdr::xvector<Operation> ops;

        // for now we have every pregenerated account except the source account
        // trust everything for some assets issued by account indexed 1...
        // NUMBER_OF_ASSETS_TO_ISSUE. We also distribute some of these assets to
        // everyone so that they can make trades, payments, etc. We start with 1
        // since we use 0 as source account for transactions and don't want that
        // account to trust any issuer or receive non-native assets
        for (int i = 1; i < mNumAccounts; ++i)
        {
            auto const account = FuzzUtils::makePublicKey(i);

            // start with 1 since we use 0 as source account for transactions
            // and don't want that account to be an issuer
            for (int j = 1; j < FuzzUtils::NUMBER_OF_ASSETS_TO_ISSUE + 1; ++j)
            {
                auto const asset = FuzzUtils::makeAsset(j);
                auto const issuer = FuzzUtils::makePublicKey(j);
                if (i != j)
                {
                    // trust asset issuer
                    auto trustOp = txtest::changeTrust(asset, INT64_MAX);
                    trustOp.sourceAccount.activate() = account;
                    ops.emplace_back(trustOp);

                    // distribute asset
                    auto distributeOp = txtest::payment(
                        account, asset,
                        FuzzUtils::INITIAL_LUMEN_AND_ASSET_BALANCE);
                    distributeOp.sourceAccount.activate() = issuer;
                    ops.emplace_back(distributeOp);
                }
            }
        }

        // construct transaction
        auto txFramePtr = createFuzzTransactionFrame(mSourceAccountID, ops,
                                                     mApp->getNetworkID());

        txFramePtr->attemptApplication(*mApp, ltx);

        ltx.commit();
    }

    {
        // In order for certain operation combinations to be valid, we need an
        // initial order book with various offers. The order book will consist
        // of identical setups for the asset pairs:
        //      XLM - A
        //      A   - B
        //      B   - C
        //      C   - D
        // For any asset A and asset B, the generic order book setup will be as
        // follows:
        //
        // +------------+-----+------+--------+------------------------------+
        // |  Account   | Bid | Sell | Amount | Price (in terms of Sell/Bid) |
        // +------------+-----+------+--------+------------------------------+
        // | 0          | A   | B    |  1,000 | 3/2                          |
        // | 1 (issuer) | A   | B    |  5,000 | 3/2                          |
        // | 2          | A   | B    | 10,000 | 1/1                          |
        // | 3 (issuer) | B   | A    |  1,000 | 10/9                         |
        // | 4          | B   | A    |  5,000 | 10/9                         |
        // | 0          | B   | A    | 10,000 | 22/7                         |
        // +------------+-----+------+--------+------------------------------+
        //
        // This gives us a good mixture of buy and sell, issuers with offers,
        // and an account with a bid and a sell. It also gives us an initial
        // order book that is in a legal state.
        LedgerTxn ltx(ltxroot);

        xdr::xvector<Operation> ops;

        auto genOffersForPair = [&ops](Asset A, Asset B, std::vector<int> pks,
                                       LedgerTxn& ltx) {
            auto addOffer = [&ops](Asset bid, Asset sell, int pk, Price price,
                                   int64 amount) {
                auto op = txtest::manageOffer(0, bid, sell, price, amount);
                op.sourceAccount.activate() = FuzzUtils::makePublicKey(pk);
                ops.emplace_back(op);
            };

            // A -> B acc0         : 1000A (3B/2A)
            addOffer(A, B, pks[0], Price{3, 2}, 1000);

            // A -> B acc1 (ISSUER): 5000A (3B/2A)
            addOffer(A, B, pks[1], Price{3, 2}, 5000);

            // A -> B acc2         : 10000A (1B/1A)
            addOffer(A, B, pks[2], Price{1, 1}, 10000);

            // B -> A acc3 (ISSUER): 1000B (10A/9B)
            addOffer(B, A, pks[3], Price{10, 9}, 1000);

            // B -> A acc4         : 5000B (10A/9B)
            addOffer(B, A, pks[4], Price{10, 9}, 5000);

            // B -> A acc0         : 10000B (22A/7B)
            addOffer(B, A, pks[0], Price{22, 7}, 10000);
        };

        auto const& XLM = txtest::makeNativeAsset();
        auto const& ASSET_A = FuzzUtils::makeAsset(1);
        auto const& ASSET_B = FuzzUtils::makeAsset(2);
        auto const& ASSET_C = FuzzUtils::makeAsset(3);
        auto const& ASSET_D = FuzzUtils::makeAsset(4);

        genOffersForPair(XLM, ASSET_A, {13, 14, 15, 1, 12}, ltx);
        genOffersForPair(ASSET_A, ASSET_B, {11, 1, 12, 2, 10}, ltx);
        genOffersForPair(ASSET_B, ASSET_C, {13, 2, 14, 3, 15}, ltx);
        genOffersForPair(ASSET_C, ASSET_D, {6, 3, 7, 4, 8}, ltx);

        // construct transaction
        auto txFramePtr = createFuzzTransactionFrame(mSourceAccountID, ops,
                                                     mApp->getNetworkID());

        txFramePtr->attemptApplication(*mApp, ltx);

        ltx.commit();
    }

    // commit this to the ledger so that we have a starting, persistent
    // state to fuzz test against
    ltxroot.commit();
}

void
TransactionFuzzer::inject(XDRInputFileStream& in)
{
    // for tryRead, in case of fuzzer creating an ill-formed xdr, generate an
    // xdr that will trigger a non-execution path so that the fuzzer realizes it
    // has hit an uninteresting case
    auto tryRead = [&in](xdr::xvector<Operation>& m) {
        try
        {
            return in.readOne(m);
        }
        catch (...)
        {
            m.clear(); // we ignore transactions with 0 operations
            return true;
        }
    };

    xdr::xvector<Operation> ops;
    while (tryRead(ops))
    {
        // limit operations per transaction to limit size of fuzzed input
        if (ops.size() < 1 || ops.size() > FuzzUtils::FUZZER_MAX_OPERATIONS)
        {
            return;
        }

        // construct transaction
        auto txFramePtr = createFuzzTransactionFrame(mSourceAccountID, ops,
                                                     mApp->getNetworkID());

        {
            resetTxInternalState(*mApp);
            LedgerTxn ltx(mApp->getLedgerTxnRoot());

            // attempt to apply transaction
            txFramePtr->attemptApplication(*mApp, ltx);
        }
        resetTxInternalState(*mApp);
    }
}

int
TransactionFuzzer::xdrSizeLimit()
{
    // path_payments are the largest operations, where 100k 1-3 operation
    // transactions (more correctly xdr::xvector's) with path_payments averaged
    // to 294 bytes, so 0x500 should be fine
    return 0x500;
}

#define FUZZER_INITIAL_CORPUS_OPERATION_GEN_UPPERBOUND 128
void
TransactionFuzzer::genFuzz(std::string const& filename)
{
    XDROutputFileStream out(/*doFsync=*/false);
    out.open(filename);
    autocheck::generator<Operation> gen;
    xdr::xvector<Operation> ops;
    auto const numops =
        autocheck::generator<uint>()(FuzzUtils::FUZZER_MAX_OPERATIONS);
    for (int i = 0; i < numops; ++i)
    {
        Operation op = gen(FUZZER_INITIAL_CORPUS_OPERATION_GEN_UPPERBOUND);
        ops.emplace_back(op);
    }

    out.writeOne(ops);
}

void
OverlayFuzzer::initialize()
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    mSimulation = std::make_shared<Simulation>(Simulation::OVER_LOOPBACK,
                                               networkID, getFuzzConfig);

    SIMULATION_CREATE_NODE(10);
    SIMULATION_CREATE_NODE(11);

    SCPQuorumSet qSet0;
    qSet0.threshold = 2;
    qSet0.validators.push_back(v10NodeID);
    qSet0.validators.push_back(v11NodeID);

    mSimulation->addNode(v10SecretKey, qSet0);
    mSimulation->addNode(v11SecretKey, qSet0);

    mSimulation->addPendingConnection(v10SecretKey.getPublicKey(),
                                      v11SecretKey.getPublicKey());

    mSimulation->startAllNodes();

    // crank until nodes are connected
    mSimulation->crankUntil(
        [&]() {
            auto nodes = mSimulation->getNodes();
            auto numberOfSimulationConnections =
                nodes[ACCEPTOR_INDEX]
                    ->getOverlayManager()
                    .getAuthenticatedPeersCount() +
                nodes[INITIATOR_INDEX]
                    ->getOverlayManager()
                    .getAuthenticatedPeersCount();
            return numberOfSimulationConnections == 2;
        },
        std::chrono::milliseconds{500}, false);
}

void
OverlayFuzzer::inject(XDRInputFileStream& in)
{
    // see note on TransactionFuzzer's tryRead above
    auto tryRead = [&in](StellarMessage& m) {
        try
        {
            return in.readOne(m);
        }
        catch (...)
        {
            m.type(HELLO); // we ignore HELLO messages
            return true;
        }
    };

    StellarMessage msg;
    while (tryRead(msg))
    {
        if (isBadOverlayFuzzerInput(msg))
        {
            return;
        }

        auto nodeids = mSimulation->getNodeIDs();
        auto loopbackPeerConnection = mSimulation->getLoopbackConnection(
            nodeids[INITIATOR_INDEX], nodeids[ACCEPTOR_INDEX]);

        auto initiator = loopbackPeerConnection->getInitiator();
        auto acceptor = loopbackPeerConnection->getAcceptor();

        initiator->getApp().getClock().postToCurrentCrank(
            [initiator, msg]() { initiator->Peer::sendMessage(msg); });

        mSimulation->crankForAtMost(std::chrono::milliseconds{500}, false);

        // clear all queues and cancel all events
        initiator->clearInAndOutQueues();
        acceptor->clearInAndOutQueues();

        while (initiator->getApp().getClock().cancelAllEvents())
            ;
        while (acceptor->getApp().getClock().cancelAllEvents())
            ;
    }
}

int
OverlayFuzzer::xdrSizeLimit()
{
    return MAX_MESSAGE_SIZE;
}

#define FUZZER_INITIAL_CORPUS_MESSAGE_GEN_UPPERBOUND 16
void
OverlayFuzzer::genFuzz(std::string const& filename)
{
    XDROutputFileStream out(/*doFsync=*/false);
    out.open(filename);
    autocheck::generator<StellarMessage> gen;
    StellarMessage m(gen(FUZZER_INITIAL_CORPUS_MESSAGE_GEN_UPPERBOUND));
    while (isBadOverlayFuzzerInput(m))
    {
        m = gen(FUZZER_INITIAL_CORPUS_MESSAGE_GEN_UPPERBOUND);
    }
    out.writeOne(m);
}
}

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
namespace xdr
{
template <>
void
generator_t::operator()(stellar::PublicKey& t) const
{
    // generate public keys such that it is zero'd out except the last byte,
    // hence for ED25519 public keys, an uint256 defined as opaque[32] set the
    // 31st indexed byte. Furthermore, utilize only the first few bits of the
    // 32nd byte, allowing us to generate these accounts against a pregeneration
    // of such accountID's.Also, note that NUMBER_OF_PREGENERATED_ACCOUNTS here
    // creates an off-by-one error, when generating acounts we use
    // [0,NUMBER_OF_PREGENERATED_ACCOUNTS) exclusive versus here we use
    // [0,NUMBER_OF_PREGENERATED_ACCOUNTS] inclusive. This allows us to generate
    // some transactions with a source account/destination account that does
    // not exist.
    t.ed25519().at(31) = autocheck::generator<uint>()(
        stellar::FuzzUtils::NUMBER_OF_PREGENERATED_ACCOUNTS);
}
} // namespace xdr
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION