// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "main/Application.h"
#include "main/Config.h"
#include "scp/SCP.h"
#include "simulation/Simulation.h"
#include "test/TestAccount.h"
#include "test/test.h"

#include "crypto/SHA.h"
#include "database/Database.h"
#include "ledger/LedgerHeaderFrame.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/CommandHandler.h"
#include "overlay/OverlayManager.h"
#include "simulation/Simulation.h"
#include "test/TxTests.h"

#include "xdrpp/marshal.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("standalone", "[herder]")
{
    SIMULATION_CREATE_NODE(0);

    Config cfg(getTestConfig());

    cfg.NODE_SEED = v0SecretKey;

    cfg.QUORUM_SET.threshold = 1;
    cfg.QUORUM_SET.validators.clear();
    cfg.QUORUM_SET.validators.push_back(v0NodeID);

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    Hash const& networkID = app->getNetworkID();
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");

    const int64_t paymentAmount = app->getLedgerManager().getMinBalance(0);

    SECTION("basic ledger close on valid txs")
    {
        bool stop = false;
        VirtualTimer setupTimer(*app);
        VirtualTimer checkTimer(*app);

        auto check = [&](asio::error_code const& error) {
            stop = true;

            REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() > 2);

            AccountFrame::pointer a1Account, b1Account;
            a1Account = loadAccount(a1, *app);
            b1Account = loadAccount(b1, *app);
            REQUIRE(a1Account->getBalance() == paymentAmount);
            REQUIRE(b1Account->getBalance() == paymentAmount);
        };

        auto setup = [&](asio::error_code const& error) {
            // create accounts
            TransactionFramePtr txFrameA1 = createCreateAccountTx(
                networkID, root, a1, root.nextSequenceNumber(), paymentAmount);
            TransactionFramePtr txFrameA2 = createCreateAccountTx(
                networkID, root, b1, root.nextSequenceNumber(), paymentAmount);

            REQUIRE(app->getHerder().recvTransaction(txFrameA1) ==
                    Herder::TX_STATUS_PENDING);
            REQUIRE(app->getHerder().recvTransaction(txFrameA2) ==
                    Herder::TX_STATUS_PENDING);
        };

        setupTimer.expires_from_now(std::chrono::seconds(0));
        setupTimer.async_wait(setup);

        checkTimer.expires_from_now(Herder::EXP_LEDGER_TIMESPAN_SECONDS +
                                    std::chrono::seconds(1));
        checkTimer.async_wait(check);

        while (!stop)
        {
            app->getClock().crank(true);
        }

        SECTION("Queue processing test")
        {
            app->getCommandHandler().manualCmd("maintenance?queue=true");

            while (app->getLedgerManager().getLastClosedLedgerNum() <
                   (app->getHistoryManager().getCheckpointFrequency() + 5))
            {
                app->getClock().crank(true);
            }

            app->getCommandHandler().manualCmd("setcursor?id=A1&cursor=1");
            app->getCommandHandler().manualCmd("maintenance?queue=true");
            auto& db = app->getDatabase();
            auto& sess = db.getSession();
            LedgerHeaderFrame::pointer lh;

            app->getCommandHandler().manualCmd("setcursor?id=A2&cursor=3");
            app->getCommandHandler().manualCmd("maintenance?queue=true");
            lh = LedgerHeaderFrame::loadBySequence(2, db, sess);
            REQUIRE(!!lh);

            app->getCommandHandler().manualCmd("setcursor?id=A1&cursor=2");
            // this should delete items older than sequence 2
            app->getCommandHandler().manualCmd("maintenance?queue=true");
            lh = LedgerHeaderFrame::loadBySequence(2, db, sess);
            REQUIRE(!lh);
            lh = LedgerHeaderFrame::loadBySequence(3, db, sess);
            REQUIRE(!!lh);

            // this should delete items older than sequence 3
            SECTION("set min to 3 by update")
            {
                app->getCommandHandler().manualCmd("setcursor?id=A1&cursor=3");
                app->getCommandHandler().manualCmd("maintenance?queue=true");
                lh = LedgerHeaderFrame::loadBySequence(3, db, sess);
                REQUIRE(!lh);
            }
            SECTION("set min to 3 by deletion")
            {
                app->getCommandHandler().manualCmd("dropcursor?id=A1");
                app->getCommandHandler().manualCmd("maintenance?queue=true");
                lh = LedgerHeaderFrame::loadBySequence(3, db, sess);
                REQUIRE(!lh);
            }
        }
    }
}

// see if we flood at the right times
//  invalid tx
//  normal tx
//  tx with bad seq num
//  account can't pay for all the tx
//  account has just enough for all the tx
//  tx from account not in the DB
TEST_CASE("recvTx", "[herder]")
{
}

TEST_CASE("txset", "[herder]")
{
    Config cfg(getTestConfig());

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    Hash const& networkID = app->getNetworkID();
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);

    const int nbAccounts = 2;
    const int nbTransactions = 5;
    SecretKey accounts[nbAccounts];

    const int64_t paymentAmount = app->getLedgerManager().getMinBalance(0);

    int64_t amountPop =
        nbAccounts * nbTransactions * app->getLedgerManager().getTxFee() +
        paymentAmount;

    auto sourceAccount = root.create("source", amountPop);

    std::vector<std::vector<TransactionFramePtr>> transactions;

    for (int i = 0; i < nbAccounts; i++)
    {
        std::string accountName = "A";
        accountName += '0' + (char)i;
        accounts[i] = getAccount(accountName.c_str());
        transactions.push_back(std::vector<TransactionFramePtr>());
        for (int j = 0; j < nbTransactions; j++)
        {
            if (j == 0)
            {
                transactions[i].emplace_back(createCreateAccountTx(
                    networkID, sourceAccount, accounts[i],
                    sourceAccount.nextSequenceNumber(), paymentAmount));
            }
            else
            {
                transactions[i].emplace_back(createPaymentTx(
                    networkID, sourceAccount, accounts[i],
                    sourceAccount.nextSequenceNumber(), paymentAmount));
            }
        }
    }

    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
        app->getLedgerManager().getLastClosedLedgerHeader().hash);

    for (auto& txs : transactions)
    {
        for (auto& tx : txs)
        {
            txSet->add(tx);
        }
    }

    SECTION("order check")
    {
        txSet->sortForHash();

        SECTION("success")
        {
            REQUIRE(txSet->checkValid(*app));

            std::vector<TransactionFramePtr> removed;
            txSet->trimInvalid(*app, removed);
            REQUIRE(txSet->checkValid(*app));
        }
        SECTION("out of order")
        {
            std::swap(txSet->mTransactions[0], txSet->mTransactions[1]);
            REQUIRE(!txSet->checkValid(*app));

            std::vector<TransactionFramePtr> removed;
            txSet->trimInvalid(*app, removed);
            REQUIRE(txSet->checkValid(*app));
        }
    }
    SECTION("invalid tx")
    {
        SECTION("no user")
        {
            txSet->add(createPaymentTx(networkID, accounts[0], root, 1,
                                       paymentAmount));
            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app));

            std::vector<TransactionFramePtr> removed;
            txSet->trimInvalid(*app, removed);
            REQUIRE(txSet->checkValid(*app));
        }
        SECTION("sequence gap")
        {
            SECTION("gap after")
            {
                txSet->add(createPaymentTx(
                    networkID, sourceAccount, accounts[0],
                    sourceAccount.getLastSequenceNumber() + 5, paymentAmount));
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app));

                std::vector<TransactionFramePtr> removed;
                txSet->trimInvalid(*app, removed);
                REQUIRE(txSet->checkValid(*app));
            }
            SECTION("gap begin")
            {
                txSet->mTransactions.erase(txSet->mTransactions.begin());
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app));

                std::vector<TransactionFramePtr> removed;
                txSet->trimInvalid(*app, removed);
                REQUIRE(txSet->checkValid(*app));
            }
            SECTION("gap middle")
            {
                txSet->mTransactions.erase(txSet->mTransactions.begin() + 3);
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app));

                std::vector<TransactionFramePtr> removed;
                txSet->trimInvalid(*app, removed);
                REQUIRE(txSet->checkValid(*app));
            }
        }
        SECTION("insuficient balance")
        {
            // extra transaction would push the account below the reserve
            txSet->add(createPaymentTx(networkID, sourceAccount, accounts[0],
                                       sourceAccount.nextSequenceNumber(),
                                       paymentAmount));
            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app));

            std::vector<TransactionFramePtr> removed;
            txSet->trimInvalid(*app, removed);
            REQUIRE(txSet->checkValid(*app));
        }
    }
}

// under surge
// over surge
// make sure it drops the correct txs
// txs with high fee but low ratio
// txs from same account high ratio with high seq
TEST_CASE("surge", "[herder]")
{
    Config cfg(getTestConfig());
    cfg.DESIRED_MAX_TX_PER_LEDGER = 5;

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    Hash const& networkID = app->getNetworkID();
    app->start();

    auto& lm = app->getLedgerManager();

    app->getLedgerManager().getCurrentLedgerHeader().maxTxSetSize =
        cfg.DESIRED_MAX_TX_PER_LEDGER;

    // set up world
    auto root = TestAccount::createRoot(*app);

    auto destAccount = root.create("destAccount", 500000000);
    auto accountB = root.create("accountB", 5000000000);
    auto accountC = root.create("accountC", 5000000000);

    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
        app->getLedgerManager().getLastClosedLedgerHeader().hash);

    SECTION("over surge")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            txSet->add(createPaymentTx(networkID, root, destAccount,
                                       root.nextSequenceNumber(), n + 10));
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(lm);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
    }

    SECTION("over surge random")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            txSet->add(createPaymentTx(networkID, root, destAccount,
                                       root.nextSequenceNumber(), n + 10));
        }
        random_shuffle(txSet->mTransactions.begin(),
                       txSet->mTransactions.end());
        txSet->sortForHash();
        txSet->surgePricingFilter(lm);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
    }

    SECTION("one account paying more")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            txSet->add(createPaymentTx(networkID, root, destAccount,
                                       root.nextSequenceNumber(), n + 10));
            auto tx = createPaymentTx(networkID, accountB, destAccount,
                                      accountB.nextSequenceNumber(), n + 10);
            tx->getEnvelope().tx.fee = tx->getEnvelope().tx.fee * 2;
            txSet->add(tx);
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(lm);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
        for (auto& tx : txSet->mTransactions)
        {
            REQUIRE(tx->getSourceID() == accountB.getPublicKey());
        }
    }

    SECTION("one account paying more except for one tx")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            auto tx = createPaymentTx(networkID, root, destAccount,
                                      root.nextSequenceNumber(), n + 10);
            tx->getEnvelope().tx.fee = tx->getEnvelope().tx.fee * 2;
            txSet->add(tx);

            tx = createPaymentTx(networkID, accountB, destAccount,
                                 accountB.nextSequenceNumber(), n + 10);
            if (n != 1)
                tx->getEnvelope().tx.fee = tx->getEnvelope().tx.fee * 3;
            txSet->add(tx);
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(lm);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
        for (auto& tx : txSet->mTransactions)
        {
            REQUIRE(tx->getSourceID() == root.getPublicKey());
        }
    }

    SECTION("a lot of txs")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 30; n++)
        {
            txSet->add(createPaymentTx(networkID, root, destAccount,
                                       root.nextSequenceNumber(), n + 10));
            txSet->add(createPaymentTx(networkID, accountB, destAccount,
                                       accountB.nextSequenceNumber(), n + 10));
            txSet->add(createPaymentTx(networkID, accountC, destAccount,
                                       accountC.nextSequenceNumber(), n + 10));
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(lm);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
    }
}

TEST_CASE("SCP Driver", "[herder]")
{
    Config cfg(getTestConfig());
    cfg.DESIRED_MAX_TX_PER_LEDGER = 5;

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    Hash const& networkID = app->getNetworkID();
    app->start();

    app->getLedgerManager().getCurrentLedgerHeader().maxTxSetSize =
        cfg.DESIRED_MAX_TX_PER_LEDGER;

    auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();

    auto root = TestAccount::createRoot(*app);
    SecretKey a1 = getAccount("A");

    using TxPair = std::pair<Value, TxSetFramePtr>;
    auto makeTxPair = [](TxSetFramePtr txSet, uint64_t closeTime) {
        txSet->sortForHash();
        auto sv = StellarValue{txSet->getContentsHash(), closeTime,
                               emptyUpgradeSteps, 0};
        auto v = xdr::xdr_to_opaque(sv);

        return TxPair{v, txSet};
    };
    auto makeEnvelope = [&root](TxPair const& p, Hash qSetHash,
                                uint64_t slotIndex) {
        // herder must want the TxSet before receiving it, so we are sending it
        // fake envelope
        auto envelope = SCPEnvelope{};
        envelope.statement.slotIndex = slotIndex;
        envelope.statement.pledges.type(SCP_ST_PREPARE);
        envelope.statement.pledges.prepare().ballot.value = p.first;
        envelope.statement.pledges.prepare().quorumSetHash = qSetHash;
        envelope.signature =
            root.getSecretKey().sign(xdr::xdr_to_opaque(envelope.statement));
        return envelope;
    };
    auto addTransactions = [&](TxSetFramePtr txSet, int n) {
        txSet->mTransactions.resize(n);
        std::generate(std::begin(txSet->mTransactions),
                      std::end(txSet->mTransactions), [&]() {
                          return createCreateAccountTx(
                              networkID, root, a1, root.nextSequenceNumber(),
                              10000000);
                      });
    };
    auto makeTransactions = [&](Hash hash, int n) {
        auto result = std::make_shared<TxSetFrame>(hash);
        addTransactions(result, n);
        return result;
    };

    SECTION("combineCandidates")
    {
        auto& herder = static_cast<HerderImpl&>(app->getHerder());

        std::set<Value> candidates;

        auto addToCandidates = [&](TxPair const& p) {
            candidates.emplace(p.first);
            auto envelope = makeEnvelope(p, {}, herder.getCurrentLedgerSeq());
            REQUIRE(herder.recvSCPEnvelope(envelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(p.second->getContentsHash(), *p.second));
        };

        TxSetFramePtr txSet0 = makeTransactions(lcl.hash, 0);
        addToCandidates(makeTxPair(txSet0, 100));

        Value v;
        StellarValue sv;

        v = herder.combineCandidates(1, candidates);
        xdr::xdr_from_opaque(v, sv);
        REQUIRE(sv.closeTime == 100);
        REQUIRE(sv.txSetHash == txSet0->getContentsHash());

        TxSetFramePtr txSet1 = makeTransactions(lcl.hash, 10);

        addToCandidates(makeTxPair(txSet1, 10));
        v = herder.combineCandidates(1, candidates);
        xdr::xdr_from_opaque(v, sv);
        REQUIRE(sv.closeTime == 100);
        REQUIRE(sv.txSetHash == txSet1->getContentsHash());

        TxSetFramePtr txSet2 = makeTransactions(lcl.hash, 5);

        addToCandidates(makeTxPair(txSet2, 1000));
        v = herder.combineCandidates(1, candidates);
        xdr::xdr_from_opaque(v, sv);
        REQUIRE(sv.closeTime == 1000);
        REQUIRE(sv.txSetHash == txSet1->getContentsHash());
    }

    SECTION("accept qset and txset")
    {
        auto makePublicKey = [](int i) {
            auto hash = sha256("NODE_SEED_" + std::to_string(i));
            auto secretKey = SecretKey::fromSeed(hash);
            return secretKey.getPublicKey();
        };

        auto makeSingleton = [](const PublicKey& key) {
            auto result = SCPQuorumSet{};
            result.threshold = 1;
            result.validators.push_back(key);
            return result;
        };

        auto keys = std::vector<PublicKey>{};
        for (auto i = 0; i < 1001; i++)
        {
            keys.push_back(makePublicKey(i));
        }

        auto saneQSet1 = makeSingleton(keys[0]);
        auto saneQSet1Hash = sha256(xdr::xdr_to_opaque(saneQSet1));
        auto saneQSet2 = makeSingleton(keys[1]);
        auto saneQSet2Hash = sha256(xdr::xdr_to_opaque(saneQSet2));

        auto bigQSet = SCPQuorumSet{};
        bigQSet.threshold = 1;
        bigQSet.validators.push_back(keys[0]);
        for (auto i = 0; i < 10; i++)
        {
            bigQSet.innerSets.push_back({});
            bigQSet.innerSets.back().threshold = 1;
            for (auto j = i * 100 + 1; j <= (i + 1) * 100; j++)
                bigQSet.innerSets.back().validators.push_back(keys[j]);
        }
        auto bigQSetHash = sha256(xdr::xdr_to_opaque(bigQSet));

        auto& herder = static_cast<HerderImpl&>(app->getHerder());
        auto transactions1 = makeTransactions(lcl.hash, 50);
        auto transactions2 = makeTransactions(lcl.hash, 40);
        auto p1 = makeTxPair(transactions1, 10);
        auto p2 = makeTxPair(transactions1, 10);
        auto saneEnvelopeQ1T1 =
            makeEnvelope(p1, saneQSet1Hash, herder.getCurrentLedgerSeq());
        auto saneEnvelopeQ1T2 =
            makeEnvelope(p2, saneQSet1Hash, herder.getCurrentLedgerSeq());
        auto saneEnvelopeQ2T1 =
            makeEnvelope(p1, saneQSet2Hash, herder.getCurrentLedgerSeq());
        auto bigEnvelope =
            makeEnvelope(p1, bigQSetHash, herder.getCurrentLedgerSeq());

        SECTION("return FETCHING until fetched")
        {
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), *p1.second));
            // will not return ENVELOPE_STATUS_READY as the recvSCPEnvelope() is
            // called internally
            // when QSet and TxSet are both received
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_PROCESSED);
        }

        SECTION("only accepts qset once")
        {
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            REQUIRE(!herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));

            SECTION("when re-receiving the same envelope")
            {
                REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(!herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            }

            SECTION("when receiving different envelope with the same qset")
            {
                REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T2) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(!herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            }
        }

        SECTION("only accepts txset once")
        {
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), *p1.second));

            SECTION("when re-receiving the same envelope")
            {
                REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(!herder.recvTxSet(p1.second->getContentsHash(),
                                          *p1.second));
            }

            SECTION("when receiving different envelope with the same txset")
            {
                REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ2T1) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(!herder.recvTxSet(p1.second->getContentsHash(),
                                          *p1.second));
            }
        }

        SECTION("do not accept unasked qset")
        {
            REQUIRE(!herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            REQUIRE(!herder.recvSCPQuorumSet(saneQSet2Hash, saneQSet2));
            REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
        }

        SECTION("do not accept unasked txset")
        {
            REQUIRE(
                !herder.recvTxSet(p1.second->getContentsHash(), *p1.second));
            REQUIRE(
                !herder.recvTxSet(p2.second->getContentsHash(), *p2.second));
        }

        SECTION("do not accept not sane qset")
        {
            REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
        }

        SECTION("do not accept txset from envelope discarded because of unsane "
                "qset")
        {
            REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
            REQUIRE(
                !herder.recvTxSet(p1.second->getContentsHash(), *p1.second));
        }

        SECTION(
            "accept txset from envelope with unsane qset before receiving qset")
        {
            REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), *p1.second));
            REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
        }

        SECTION("accept txset from envelopes with both valid and unsane qset")
        {
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
            REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), *p1.second));
        }
    }
}

TEST_CASE("SCP State", "[herder]")
{
    SecretKey nodeKeys[3];
    PublicKey nodeIDs[3];

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer sim =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    Config nodeCfgs[3];

    // Normally ledger should externalize in EXP_LEDGER_TIMESPAN_SECONDS
    // but for "Force SCP" test there are 3 nodes and only 2 have previous
    // ledger state. However it is possible that nomination protocol will
    // choose last node as leader for first few rounds. New ledger will only
    // be externalized when first or second node are choosen as round leaders.
    // It some cases it can take more time than expected. Probability of that
    // is pretty low, but high enough that it forced us to rerun tests from
    // time to time to pass that one case.
    //
    // After changing node ids generated here from random to deterministics
    // this problem goes away, as the leader selection protocol uses node id
    // and round id for selecting leader.
    for (int i = 0; i < 3; i++)
    {
        nodeKeys[i] = SecretKey::fromSeed(sha256("Node_" + std::to_string(i)));
        nodeIDs[i] = nodeKeys[i].getPublicKey();
        nodeCfgs[i] =
            getTestConfig(i + 1, Config::TestDbMode::TESTDB_ON_DISK_SQLITE);
    }

    VirtualClock* clock = &sim->getClock();

    LedgerHeaderHistoryEntry lcl;

    auto doTest = [&](bool forceSCP) {
        // add node0 and node1, in lockstep
        {
            SCPQuorumSet qSet;
            qSet.threshold = 2;
            qSet.validators.push_back(nodeIDs[0]);
            qSet.validators.push_back(nodeIDs[1]);

            sim->addNode(nodeKeys[0], qSet, *clock, &nodeCfgs[0]);
            sim->addNode(nodeKeys[1], qSet, *clock, &nodeCfgs[1]);
            sim->addPendingConnection(nodeIDs[0], nodeIDs[1]);
        }

        sim->startAllNodes();
        // wait to close exactly once

        sim->crankUntil([&]() { return sim->haveAllExternalized(2, 1); },
                        std::chrono::seconds(1), true);

        REQUIRE(sim->getNode(nodeIDs[0])
                    ->getLedgerManager()
                    .getLastClosedLedgerNum() == 2);
        REQUIRE(sim->getNode(nodeIDs[1])
                    ->getLedgerManager()
                    .getLastClosedLedgerNum() == 2);

        lcl = sim->getNode(nodeIDs[0])
                  ->getLedgerManager()
                  .getLastClosedLedgerHeader();

        // adjust configs for a clean restart
        for (int i = 0; i < 2; i++)
        {
            nodeCfgs[i] = sim->getNode(nodeIDs[i])->getConfig();
            nodeCfgs[i].FORCE_SCP = forceSCP;
        }

        // restart simulation
        sim.reset();

        sim =
            std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);
        clock = &sim->getClock();

        // start a new node that will switch to whatever node0 & node1 says
        SCPQuorumSet qSetAll;
        qSetAll.threshold = 2;
        for (int i = 0; i < 3; i++)
        {
            qSetAll.validators.push_back(nodeIDs[i]);
        }
        sim->addNode(nodeKeys[2], qSetAll, *clock, &nodeCfgs[2]);
        sim->getNode(nodeIDs[2])->start();

        // crank a bit (nothing should happen, node 2 is waiting for SCP
        // messages)
        sim->crankForAtLeast(std::chrono::seconds(1), false);

        REQUIRE(sim->getNode(nodeIDs[2])
                    ->getLedgerManager()
                    .getLastClosedLedgerNum() == 1);

        // start up node 0 and 1 again
        // nodes 0 and 1 have lost their SCP state as they got restarted
        // yet they should have their own last statements that should be
        // forwarded to node 2 when they connect to it
        // causing node 2 to externalize ledger #2

        sim->addNode(nodeKeys[0], qSetAll, *clock, &nodeCfgs[0], false);
        sim->addNode(nodeKeys[1], qSetAll, *clock, &nodeCfgs[1], false);
        sim->getNode(nodeIDs[0])->start();
        sim->getNode(nodeIDs[1])->start();

        sim->addConnection(nodeIDs[0], nodeIDs[2]);
        sim->addConnection(nodeIDs[1], nodeIDs[2]);
    };

    SECTION("Force SCP")
    {
        doTest(true);

        // then let the nodes run a bit more, they should all externalize the
        // next ledger
        sim->crankUntil([&]() { return sim->haveAllExternalized(3, 2); },
                        Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

        // nodes are at least on ledger 3 (some may be on 4)
        for (int i = 0; i <= 2; i++)
        {
            auto const& actual = sim->getNode(nodeIDs[i])
                                     ->getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header;
            if (actual.ledgerSeq == 3)
            {
                REQUIRE(actual.previousLedgerHash == lcl.hash);
            }
        }
    }

    SECTION("No Force SCP")
    {
        // node 0 and 1 don't try to close, causing all nodes
        // to get stuck at ledger #2
        doTest(false);

        sim->crankUntil(
            [&]() {
                return sim->getNode(nodeIDs[2])
                           ->getLedgerManager()
                           .getLastClosedLedgerNum() == 2;
            },
            std::chrono::seconds(1), false);

        for (int i = 0; i <= 2; i++)
        {
            auto const& actual = sim->getNode(nodeIDs[i])
                                     ->getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header;
            REQUIRE(actual == lcl.header);
        }
    }
}
