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
        resetResults(ltx.getHeader(), 0, true);

        // attempt application of transaction without accounting for sequence
        // number, processing the fee, or committing the LedgerTxn
        SignatureChecker signatureChecker{
            ltx.loadHeader().current().ledgerVersion, getContentsHash(),
            mEnvelope.v0().signatures};
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
        TransactionMeta tm(2);
        applyOperations(signatureChecker, app, ltx, tm);
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
    txEnv.v0().tx.sourceAccountEd25519 = sourceAccountID.ed25519();
    txEnv.v0().tx.fee = 0;
    txEnv.v0().tx.seqNum = 1;
    std::copy(std::begin(ops), std::end(ops),
              std::back_inserter(txEnv.v0().tx.operations));

    std::shared_ptr<FuzzTransactionFrame> res =
        std::make_shared<FuzzTransactionFrame>(networkID, txEnv);
    return res;
}

bool
isBadTransactionFuzzerInput(Operation const& op)
{
    return !(op.body.type() == ACCOUNT_MERGE) &&
           !(op.body.type() == PAYMENT &&
             op.body.paymentOp().asset.type() == ASSET_TYPE_NATIVE);
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
    LedgerTxn ltx(mApp->getLedgerTxnRoot());

    // setup the state, for this we only need to pregenerate some accounts. For
    // now we create mNumAccounts accounts, or enough to fill the first few bits
    // such that we have a pregenerated account for the last few bits of the
    // 32nd byte of a public key, thus account creation is over a deterministic
    // range of public keys
    for (unsigned int i = 0; i < mNumAccounts; ++i)
    {
        PublicKey publicKey;
        uint256 accountID;
        // NB: need to map to more bytes if we have more accounts at some point
        assert(i <= std::numeric_limits<uint8_t>::max());
        accountID.at(31) = static_cast<uint8_t>(i);
        publicKey.ed25519() = accountID;

        // manually construct ledger entries, "creating" each account
        LedgerEntry newAccountEntry;
        newAccountEntry.data.type(ACCOUNT);
        auto& newAccount = newAccountEntry.data.account();
        newAccount.thresholds[0] = 1;
        newAccount.accountID = publicKey;
        newAccount.seqNum = 0;
        // to create "interesting" balances we utilize powers of 2
        newAccount.balance = 2 << i;
        ltx.create(newAccountEntry);

        // select the first pregenerated account to be the hard coded source
        // account for all transactions
        if (i == 0)
            mSourceAccountID = publicKey;
    }

    // commit these to the ledger so that we have a starting, persistent state
    // to fuzz test against -- should be the only stateful/effectful action
    ltx.commit();
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
        auto wellSizedTx = ops.size() > 0 && ops.size() < 6;
        if (std::any_of(ops.begin(), ops.end(), isBadTransactionFuzzerInput) ||
            !wellSizedTx)
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

#define FUZZER_INITIAL_CORPUS_MAX_OPERATIONS 5
#define FUZZER_INITIAL_CORPUS_OPERATION_GEN_UPPERBOUND 128
void
TransactionFuzzer::genFuzz(std::string const& filename)
{
    XDROutputFileStream out(mApp->getClock().getIOContext(),
                            /*doFsync=*/false);
    out.open(filename);
    autocheck::generator<Operation> gen;
    xdr::xvector<Operation> ops;
    auto numops = autocheck::generator<unsigned int>()(
        FUZZER_INITIAL_CORPUS_MAX_OPERATIONS);
    for (unsigned int i = 0; i < numops; ++i)
    {
        Operation op = gen(FUZZER_INITIAL_CORPUS_OPERATION_GEN_UPPERBOUND);
        while (isBadTransactionFuzzerInput(op))
        {
            op = gen(FUZZER_INITIAL_CORPUS_OPERATION_GEN_UPPERBOUND);
        }
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

        initiator->getApp().postOnMainThread(
            [initiator, msg]() { initiator->Peer::sendMessage(msg); },
            "OverlayFuzzer");

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
    VirtualClock clock;
    XDROutputFileStream out(clock.getIOContext(),
                            /*doFsync=*/false);
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
    t.ed25519().at(31) = autocheck::generator<unsigned int>()(
        stellar::FuzzUtils::NUMBER_OF_PREGENERATED_ACCOUNTS);
}
} // namespace xdr
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
