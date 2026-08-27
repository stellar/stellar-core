// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketIndexUtils.h"
#include "herder/HerderImpl.h"
#include "herder/LedgerCloseData.h"
#include "herder/test/TestTxSetUtils.h"
#include "main/Application.h"
#include "main/Config.h"
#include "scp/LocalNode.h"
#include "scp/SCP.h"
#include "scp/Slot.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/JitterInjection.h"

#include "history/HistoryArchiveManager.h"
#include "history/test/HistoryTestsUtils.h"

#include "catchup/LedgerApplyManagerImpl.h"
#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "herder/HerderUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/CommandHandler.h"
#include "main/PersistentState.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/RustOverlayManager.h"
#include "test/Catch2.h"
#include "test/TxTests.h"
#include "transactions/OperationFrame.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/TransactionTestFrame.h"
#include "util/Decoder.h"
#include "util/Math.h"
#include "util/MetricsRegistry.h"
#include "util/ProtocolVersion.h"

#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "ledger/test/LedgerTestUtils.h"
#include "test/TxTests.h"
#include "xdr/Stellar-internal.h"
#include "xdr/Stellar-ledger.h"
#include "xdrpp/autocheck.h"
#include "xdrpp/marshal.h"
#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <memory>
#include <numeric>
#include <optional>

using namespace stellar;
using namespace stellar::txbridge;
using namespace stellar::txtest;
using namespace historytestutils;

static TransactionTestFramePtr
makeMultiPayment(stellar::TestAccount &destAccount, stellar::TestAccount &src,
                 int nbOps, int64 paymentBase, uint32 extraFee,
                 uint32 feeMult) {
  std::vector<stellar::Operation> ops;
  for (int i = 0; i < nbOps; i++) {
    ops.emplace_back(payment(destAccount, i + paymentBase));
  }
  auto tx = src.tx(ops);
  setFullFee(tx, static_cast<uint32_t>(tx->getFullFee()) * feeMult + extraFee);
  getSignatures(tx).clear();
  tx->addSignature(src);
  return tx;
}

static TransactionTestFramePtr makeSelfPayment(stellar::TestAccount &account,
                                               int nbOps, uint32_t fee) {
  std::vector<stellar::Operation> ops;
  for (int i = 0; i < nbOps; i++) {
    ops.emplace_back(payment(account, i + 1000));
  }
  auto tx = account.tx(ops);
  setFullFee(tx, fee);
  getSignatures(tx).clear();
  tx->addSignature(account);
  return tx;
}

static void testTxSet(uint32 protocolVersion) {
  Config cfg(getTestConfig());
  cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 15;
  cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
  cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
  VirtualClock clock;
  Application::pointer app = createTestApplication(clock, cfg);

  // set up world
  auto root = app->getRoot();

  int const nbAccounts = 3;

  std::vector<TestAccount> accounts;

  int64_t const minBalance0 = app->getLedgerManager().getLastMinBalance(0);

  int64_t accountBalance = app->getLedgerManager().getLastTxFee() + minBalance0;

  std::vector<TransactionFrameBasePtr> txs;
  auto genTx = [&]() {
    std::string accountName = fmt::format("A{}", accounts.size());
    accounts.push_back(root->create(accountName.c_str(), accountBalance));
    auto &account = accounts.back();

    // payment to self
    txs.push_back(account.tx({payment(account.getPublicKey(), 10000)}));
  };
  for (size_t i = 0; i < nbAccounts; i++) {
    genTx();
  }

  // Helper to build an unvalidated block and check validation result
  auto validateReceivedBlock =
      [&](std::vector<TransactionFrameBasePtr> const &blockTxs,
          TxSetValidationResult expectedResult) {
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                         {{std::make_pair(std::nullopt, blockTxs)}, {}}, *app,
                         ledgerHash)
                         .second;
        REQUIRE(txSet);
        REQUIRE(txSet->checkValidWithResult(*app, 0, 0) == expectedResult);
      };

  SECTION("valid set") {
    auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0).second;
    REQUIRE(txSet->sizeTxTotal() == nbAccounts);
    REQUIRE(txSet->checkValidWithResult(*app, 0, 0) ==
            TxSetValidationResult::VALID);
  }

  SECTION("too many txs") {
    while (txs.size() <= cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE * 2) {
      genTx();
    }
    auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0).second;
    REQUIRE(txSet->sizeTxTotal() == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
    REQUIRE(txSet->checkValidWithResult(*app, 0, 0) ==
            TxSetValidationResult::VALID);
  }
  SECTION("invalid tx") {
    auto diagnostics = DiagnosticEventManager::createDisabled();
    CheckValidLedgerViewWrapper ledgerView(*app);

    SECTION("no user") {
      auto newUser = TestAccount{*app, getAccount("doesnotexist")};
      auto badTx = newUser.tx({payment(*root, 1)});
      txs.push_back(badTx);

      // Individual tx check: account doesn't exist
      REQUIRE(badTx
                  ->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                               diagnostics)
                  ->getResultCode() == txNO_ACCOUNT);

      SECTION("build block") {
        TxFrameList removed;
        auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0, removed).second;
        REQUIRE(removed.size() == 1);
        REQUIRE(removed.back() == badTx);
        REQUIRE(txSet->sizeTxTotal() == nbAccounts);
        REQUIRE(txSet->checkValidWithResult(*app, 0, 0) ==
                TxSetValidationResult::VALID);
      }
      SECTION("validate block") {
        validateReceivedBlock(txs, TxSetValidationResult::TX_VALIDATION_FAILED);
      }
    }
    SECTION("sequence gap") {
      auto txPtr = std::const_pointer_cast<TransactionFrameBase>(txs[0]);
      setSeqNum(std::static_pointer_cast<TransactionTestFrame>(txPtr),
                txs[0]->getSeqNum() + 5);
      auto badTx = txs[0];

      // Individual tx check: bad sequence number
      REQUIRE(badTx
                  ->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                               diagnostics)
                  ->getResultCode() == txBAD_SEQ);

      SECTION("build block") {
        TxFrameList removed;
        auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0, removed).second;
        REQUIRE(removed.size() == 1);
        REQUIRE(removed.back() == badTx);
        REQUIRE(txSet->sizeTxTotal() == nbAccounts - 1);
        REQUIRE(txSet->checkValidWithResult(*app, 0, 0) ==
                TxSetValidationResult::VALID);
      }
      SECTION("validate block") {
        validateReceivedBlock(txs, TxSetValidationResult::TX_VALIDATION_FAILED);
      }
    }
    SECTION("insufficient balance") {
      accounts.push_back(root->create("insufficient", accountBalance - 1));
      txs.back() =
          accounts.back().tx({payment(accounts.back().getPublicKey(), 10000)});
      auto badTx = txs.back();

      // Individual tx check: insufficient balance
      // Need fresh snapshot after account creation
      CheckValidLedgerViewWrapper lsNew(*app);
      REQUIRE(
          badTx->checkValid(app->getAppConnector(), lsNew, 0, 0, 0, diagnostics)
              ->getResultCode() == txINSUFFICIENT_BALANCE);

      SECTION("build block") {
        TxFrameList removed;
        auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0, removed).second;
        REQUIRE(removed.size() == 1);
        REQUIRE(removed.back() == badTx);
        REQUIRE(txSet->sizeTxTotal() == nbAccounts - 1);
        REQUIRE(txSet->checkValidWithResult(*app, 0, 0) ==
                TxSetValidationResult::VALID);
      }
      SECTION("validate block") {
        validateReceivedBlock(txs, TxSetValidationResult::TX_VALIDATION_FAILED);
      }
    }
    SECTION("bad signature") {
      auto tx = std::static_pointer_cast<TransactionTestFrame const>(txs[0]);
      setMaxTime(tx, UINT64_MAX);
      tx->clearCached();
      auto badTx = txs[0];

      // Individual tx check: bad auth (signature invalidated by maxTime
      // change)
      REQUIRE(badTx
                  ->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                               diagnostics)
                  ->getResultCode() == txBAD_AUTH);

      SECTION("build block") {
        TxFrameList removed;
        auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0, removed).second;
        REQUIRE(removed.size() == 1);
        REQUIRE(removed.back() == badTx);
        REQUIRE(txSet->sizeTxTotal() == nbAccounts - 1);
        REQUIRE(txSet->checkValidWithResult(*app, 0, 0) ==
                TxSetValidationResult::VALID);
      }
      SECTION("validate block") {
        validateReceivedBlock(txs, TxSetValidationResult::TX_VALIDATION_FAILED);
      }
    }
    SECTION("zero ops transaction") {
      auto lclHeader = app->getLedgerManager().getLastClosedLedgerHeader();

      auto tx = transactionFromOperations(*app, root->getSecretKey(),
                                          root->nextSequenceNumber(), {}, 1000);

      SECTION("legacy tx set") {
        // This is a regression test - legacy tx sets are not allowed in
        // new protocols, but Core still accepts them and it does some
        // tx-related validation before reaching the
        // `GENERALIZED_TXSET_MISMATCH` check.
        TransactionSet txSet;
        txSet.previousLedgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        txSet.txs.push_back(tx->getEnvelope());
        auto applicableTxSet =
            TxSetXDRFrame::makeFromWire(txSet)->prepareForApply(
                *app, lclHeader.header);
        REQUIRE(applicableTxSet != nullptr);
        REQUIRE(applicableTxSet->checkValidWithResult(*app, 0, 0) ==
                TxSetValidationResult::GENERALIZED_TXSET_MISMATCH);
      }
      SECTION("generalized tx set") {
        auto txSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(std::nullopt,
                                 std::vector<TransactionFrameBasePtr>{tx})},
                 {}},
                *app, lclHeader.hash)
                .second;
        REQUIRE(txSet);
        REQUIRE(txSet->checkValidWithResult(*app, 0, 0) ==
                TxSetValidationResult::TX_VALIDATION_FAILED);
      }
    }
    SECTION("negative inclusion fee tx") {
      auto sorobanTx =
          createUploadWasmTx(*app, *root, 100, 1000, SorobanResources{});
      auto negFeeSorobanTxEnvelope = sorobanTx->getEnvelope();
      negFeeSorobanTxEnvelope.v1().tx.fee = 1;
      auto negFeeBump = feeBump(*app, *root, sorobanTx, 1,
                                /* useInclusionAsFullFee */ true);
      auto lclHeader = app->getLedgerManager().getLastClosedLedgerHeader();
      SECTION("legacy tx set") {
        // This is a regression test - legacy tx sets are not allowed in
        // new protocols, but Core still accepts them and it does some
        // tx-related validation before reaching the
        // `GENERALIZED_TXSET_MISMATCH` check.
        TransactionSet txSet;
        txSet.previousLedgerHash = lclHeader.hash;
        txSet.txs.push_back(negFeeSorobanTxEnvelope);
        auto applicableTxSet =
            TxSetXDRFrame::makeFromWire(txSet)->prepareForApply(
                *app, lclHeader.header);
        REQUIRE(applicableTxSet == nullptr);

        txSet.txs[0] = negFeeBump->getEnvelope();
        auto applicableTxSet2 =
            TxSetXDRFrame::makeFromWire(txSet)->prepareForApply(
                *app, lclHeader.header);
        REQUIRE(applicableTxSet2 == nullptr);
      }
      SECTION("generalized tx set") {
        auto txSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{},
                 {std::make_pair(
                     std::nullopt,
                     std::vector<TransactionFrameBasePtr>{
                         TransactionFrameBase::makeTransactionFromWire(
                             app->getNetworkID(), negFeeSorobanTxEnvelope)})}},
                *app, lclHeader.hash)
                .second;
        REQUIRE(txSet == nullptr);

        auto txSet2 = testtxset::makeNonValidatedGeneralizedTxSet(
                          {{},
                           {std::make_pair(std::nullopt,
                                           std::vector<TransactionFrameBasePtr>{
                                               negFeeBump})}},
                          *app, lclHeader.hash)
                          .second;
        REQUIRE(txSet2 == nullptr);
      }
    }
  }
}

static TransactionTestFramePtr transaction(Application &app,
                                           TestAccount &account,
                                           int64_t sequenceDelta,
                                           int64_t amount, uint32_t fee) {
  return transactionFromOperations(
      app, account, account.getLastSequenceNumber() + sequenceDelta,
      {payment(account.getPublicKey(), amount)}, fee);
}

static void testTxSetWithFeeBumps(uint32 protocolVersion) {
  Config cfg(getTestConfig());
  cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 14;
  cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
  VirtualClock clock;
  Application::pointer app = createTestApplication(clock, cfg);

  auto const minBalance0 = app->getLedgerManager().getLastMinBalance(0);
  auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);
  auto root = app->getRoot();
  auto account1 = root->create("a1", minBalance2);
  auto account2 = root->create("a2", minBalance2);
  auto account3 = root->create("a3", minBalance2);
  auto account4 = root->create("a4", minBalance0);
  auto account5 = root->create("a5", minBalance0);

  auto compareTxs = [](TxFrameList const &actual, TxFrameList const &expected) {
    auto actualNormalized = actual;
    auto expectedNormalized = expected;
    std::sort(actualNormalized.begin(), actualNormalized.end());
    std::sort(expectedNormalized.begin(), expectedNormalized.end());
    REQUIRE(actualNormalized == expectedNormalized);
  };

  // Helper to build an unvalidated block and check validation result
  auto validateReceivedBlock =
      [&](std::vector<TransactionFrameBasePtr> const &blockTxs,
          TxSetValidationResult expectedResult) {
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                         {{std::make_pair(std::nullopt, blockTxs)}, {}}, *app,
                         ledgerHash)
                         .second;
        REQUIRE(txSet);
        REQUIRE(txSet->checkValidWithResult(*app, 0, 0) == expectedResult);
      };

  auto diagnostics = DiagnosticEventManager::createDisabled();
  CheckValidLedgerViewWrapper ledgerView(*app);

  SECTION("invalid transaction") {
    SECTION("one fee bump") {
      auto tx1 = transaction(*app, account1, 1, 1, 100);
      auto fb1 = feeBump(*app, account2, tx1, minBalance2);

      // Individual tx check: fee bump exceeds fee source balance
      REQUIRE(fb1->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->getResultCode() == txINSUFFICIENT_BALANCE);

      SECTION("build block") {
        TxFrameList invalidTxs;
        auto txSet = makeTxSetFromTransactions({fb1}, *app, 0, 0, invalidTxs);
        compareTxs(invalidTxs, {fb1});
      }
      SECTION("validate block") {
        validateReceivedBlock({fb1},
                              TxSetValidationResult::TX_VALIDATION_FAILED);
      }
    }

    SECTION("two fee bumps with same sources, first has high fee") {
      auto tx1 = transaction(*app, account1, 1, 1, 100);
      auto fb1 = feeBump(*app, account2, tx1, minBalance2);
      auto tx2 = transaction(*app, account3, 1, 1, 100);
      auto fb2 = feeBump(*app, account2, tx2, 200);

      // Individual tx checks: first exceeds balance, second is valid
      REQUIRE(fb1->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->getResultCode() == txINSUFFICIENT_BALANCE);
      REQUIRE(fb2->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->isSuccess());

      SECTION("build block") {
        TxFrameList invalidTxs;
        auto txSet =
            makeTxSetFromTransactions({fb1, fb2}, *app, 0, 0, invalidTxs);
        // fb1 is rejected
        compareTxs(invalidTxs, {fb1});
      }
      SECTION("validate block") {
        validateReceivedBlock({fb1, fb2},
                              TxSetValidationResult::TX_VALIDATION_FAILED);
      }
    }
    SECTION("two fee bumps with same fee source, second has high fee") {
      auto tx1 = transaction(*app, account1, 1, 1, 100);
      auto fb1 = feeBump(*app, account2, tx1, 200);
      auto tx2 = transaction(*app, account3, 1, 1, 100);
      auto fb2 = feeBump(*app, account2, tx2, minBalance2);

      REQUIRE(fb1->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->isSuccess());
      REQUIRE(fb2->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->getResultCode() == txINSUFFICIENT_BALANCE);

      SECTION("build block") {
        TxFrameList invalidTxs;
        auto txSet =
            makeTxSetFromTransactions({fb1, fb2}, *app, 0, 0, invalidTxs);
        compareTxs(invalidTxs, {fb2});
      }
      SECTION("validate block") {
        validateReceivedBlock({fb1, fb2},
                              TxSetValidationResult::TX_VALIDATION_FAILED);
      }
    }
    SECTION("two fee bumps with same fee source, second malformed operation") {
      auto tx1 = transaction(*app, account1, 1, 1, 100);
      auto fb1 = feeBump(*app, account2, tx1, 200);
      auto tx2 = transaction(*app, account3, 1, -1, 100);
      auto fb2 = feeBump(*app, account2, tx2, 200);

      // Individual tx checks
      REQUIRE(fb1->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->isSuccess());
      REQUIRE(fb2->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->getResultCode() == txFEE_BUMP_INNER_FAILED);

      SECTION("build block") {
        TxFrameList invalidTxs;
        auto txSet =
            makeTxSetFromTransactions({fb1, fb2}, *app, 0, 0, invalidTxs);
        compareTxs(invalidTxs, {fb2});
      }
      SECTION("validate block") {
        validateReceivedBlock({fb1, fb2},
                              TxSetValidationResult::TX_VALIDATION_FAILED);
      }
    }
    SECTION("three fee bumps with same fee source, second malformed "
            "operation, third insufficient") {
      auto tx1 = transaction(*app, account1, 1, 1, 100);
      auto fb1 = feeBump(*app, account2, tx1, 200);
      auto tx2 = transaction(*app, account3, 1, -1, 100);
      auto fb2 = feeBump(*app, account2, tx2, 200);
      auto tx3 = transaction(*app, account4, 1, 1, 100);
      auto fb3 = feeBump(*app, account2, tx3, minBalance2 - minBalance0 - 199);

      // Individual tx checks
      REQUIRE(fb1->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->isSuccess());
      REQUIRE(fb2->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->getResultCode() == txFEE_BUMP_INNER_FAILED);
      // Individually, fb2 is valid, but with fb1 it would exceed balance
      REQUIRE(fb3->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->isSuccess());

      SECTION("build block") {
        TxFrameList invalidTxs;
        auto txSet =
            makeTxSetFromTransactions({fb1, fb2, fb3}, *app, 0, 0, invalidTxs);
        compareTxs(invalidTxs, {fb1, fb2, fb3});
      }
      SECTION("validate block") {
        validateReceivedBlock({fb1, fb2, fb3},
                              TxSetValidationResult::TX_VALIDATION_FAILED);
      }
    }
    SECTION("two fee bumps, same fee source, valid individually, combined "
            "exceed balance") {
      CheckValidLedgerViewWrapper ledgerView(*app);
      auto balanceOfFbAccount = getAvailableBalance(
          ledgerView.getLedgerHeader().current(),
          ledgerView.getAccount(account2.getPublicKey()).current());

      // Enforce balance invariance
      int64_t fee1 = 200;
      int64_t fee2 = balanceOfFbAccount - fee1 + 1;

      REQUIRE(balanceOfFbAccount < fee1 + fee2);
      REQUIRE(fee1 < balanceOfFbAccount);
      REQUIRE(fee2 < balanceOfFbAccount);

      auto tx1 = transaction(*app, account1, 1, 1, 100);
      auto fb1 = feeBump(*app, account2, tx1, fee1);
      auto tx2 = transaction(*app, account3, 1, 1, 100);
      auto fb2 = feeBump(*app, account2, tx2, fee2);
      // Individual txs are valid
      auto diagnostics = DiagnosticEventManager::createDisabled();
      REQUIRE(fb1->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->isSuccess());
      REQUIRE(fb2->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->isSuccess());

      TxFrameList invalidTxs;
      SECTION("build block") {
        auto txSet =
            makeTxSetFromTransactions({fb1, fb2}, *app, 0, 0, invalidTxs);
        // Both are marked invalid because their combined fees exceed
        // account2's balance
        compareTxs(invalidTxs, {fb1, fb2});
      }
      SECTION("validate block") {
        validateReceivedBlock({fb1, fb2},
                              TxSetValidationResult::ACCOUNT_CANT_PAY_FEE);
      }
    }
    SECTION("two Soroban fee bumps, same fee source, valid individually, "
            "combined exceed balance") {
      // Increase Soroban limits to allow multiple transactions
      modifySorobanNetworkConfig(*app, [](SorobanNetworkConfig &cfg) {
        auto mx = std::numeric_limits<uint32_t>::max();
        cfg.mLedgerMaxTxCount = mx;
        cfg.mLedgerMaxInstructions = mx;
        cfg.mLedgerMaxDiskReadBytes = mx;
        cfg.mLedgerMaxWriteBytes = mx;
        cfg.mLedgerMaxDiskReadEntries = mx;
        cfg.mLedgerMaxWriteLedgerEntries = mx;
      });

      // Create accounts with enough balance for Soroban transactions
      auto sorobanAccount1 = root->create("s1", minBalance2);
      auto sorobanAccount2 = root->create("s2", minBalance2);
      auto feeSourceAccount = root->create("fs", minBalance2);

      SorobanResources resources;
      resources.instructions = 1'000'000;
      resources.diskReadBytes = 1000;
      resources.writeBytes = 1000;

      auto sorobanTx1 = createUploadWasmTx(
          *app, sorobanAccount1, 100, DEFAULT_TEST_RESOURCE_FEE, resources);
      auto sorobanTx2 = createUploadWasmTx(
          *app, sorobanAccount2, 100, DEFAULT_TEST_RESOURCE_FEE, resources);

      CheckValidLedgerViewWrapper ledgerView(*app);
      auto balanceOfFbAccount = getAvailableBalance(
          ledgerView.getLedgerHeader().current(),
          ledgerView.getAccount(feeSourceAccount.getPublicKey()).current());

      // Set fees so that each is valid individually but combined they
      // exceed the fee source's balance
      int64_t fee1 = 200 + DEFAULT_TEST_RESOURCE_FEE;
      auto fb1 = feeBump(*app, feeSourceAccount, sorobanTx1, fee1,
                         /* useInclusionAsFullFee */ true);

      int64_t fee2 = balanceOfFbAccount - fb1->getFullFee() + 1;
      auto fb2 = feeBump(*app, feeSourceAccount, sorobanTx2, fee2,
                         /* useInclusionAsFullFee */ true);

      REQUIRE(balanceOfFbAccount < fb1->getFullFee() + fb2->getFullFee());
      REQUIRE(fb1->getFullFee() < balanceOfFbAccount);
      REQUIRE(fb2->getFullFee() < balanceOfFbAccount);

      // Individual txs are valid
      auto diagnostics = DiagnosticEventManager::createDisabled();
      REQUIRE(fb1->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->isSuccess());
      REQUIRE(fb2->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                              diagnostics)
                  ->isSuccess());

      PerPhaseTransactionList invalidPerPhase;
      invalidPerPhase.resize(2);
      SECTION("build block") {
        auto txSet = makeTxSetFromTransactions({{}, {fb1, fb2}}, *app, 0, 0,
                                               invalidPerPhase);
        // Both are marked invalid because their combined fees exceed
        // feeSourceAccount's balance
        compareTxs(invalidPerPhase[1], {fb1, fb2});
      }
      SECTION("validate block") {
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                         {{},
                          {std::make_pair(
                              std::nullopt,
                              std::vector<TransactionFrameBasePtr>{fb1, fb2})}},
                         *app, ledgerHash)
                         .second;
        REQUIRE(txSet->checkValidWithResult(*app, 0, 0) ==
                TxSetValidationResult::ACCOUNT_CANT_PAY_FEE);
      }
    }
    SECTION("cross-phase fee bumps") {
      modifySorobanNetworkConfig(*app, [](SorobanNetworkConfig &cfg) {
        auto mx = std::numeric_limits<uint32_t>::max();
        cfg.mLedgerMaxTxCount = mx;
        cfg.mLedgerMaxInstructions = mx;
        cfg.mLedgerMaxDiskReadBytes = mx;
        cfg.mLedgerMaxWriteBytes = mx;
        cfg.mLedgerMaxDiskReadEntries = mx;
        cfg.mLedgerMaxWriteLedgerEntries = mx;
      });

      auto &feeSourceAccount = account1;

      SorobanResources resources;
      resources.instructions = 1'000'000;
      resources.diskReadBytes = 1000;
      resources.writeBytes = 1000;

      auto classicTx = transaction(*app, account2, 1, 1, 100);
      auto sorobanTx = createUploadWasmTx(*app, account3, 100,
                                          DEFAULT_TEST_RESOURCE_FEE, resources);

      auto balanceOfFbAccount = feeSourceAccount.getAvailableBalance();

      auto classicFb = feeBump(*app, feeSourceAccount, classicTx, 200);
      int64_t sorobanFee = balanceOfFbAccount - classicFb->getFullFee() + 1;
      auto sorobanFb = feeBump(*app, feeSourceAccount, sorobanTx, sorobanFee,
                               /* useInclusionAsFullFee */ true);

      REQUIRE(balanceOfFbAccount <
              classicFb->getFullFee() + sorobanFb->getFullFee());
      REQUIRE(classicFb->getFullFee() < balanceOfFbAccount);
      REQUIRE(sorobanFb->getFullFee() < balanceOfFbAccount);

      auto diagnostics = DiagnosticEventManager::createDisabled();
      REQUIRE(classicFb
                  ->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                               diagnostics)
                  ->isSuccess());
      REQUIRE(sorobanFb
                  ->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0,
                               diagnostics)
                  ->isSuccess());

      PerPhaseTransactionList invalidPerPhase;
      invalidPerPhase.resize(2);
      SECTION("build block") {
        auto txSet = makeTxSetFromTransactions({{classicFb}, {sorobanFb}}, *app,
                                               0, 0, invalidPerPhase);
        compareTxs(invalidPerPhase[0], {});
        compareTxs(invalidPerPhase[1], {sorobanFb});
      }
      SECTION("validate block") {
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                         {{std::make_pair(
                              std::nullopt,
                              std::vector<TransactionFrameBasePtr>{classicFb})},
                          {std::make_pair(std::nullopt,
                                          std::vector<TransactionFrameBasePtr>{
                                              sorobanFb})}},
                         *app, ledgerHash)
                         .second;
        auto expected =
            protocolVersion >= static_cast<uint32>(ProtocolVersion::V_26)
                ? TxSetValidationResult::ACCOUNT_CANT_PAY_FEE
                : TxSetValidationResult::VALID;
        REQUIRE(txSet->checkValidWithResult(*app, 0, 0) == expected);
      }
    }
  }
}

TEST_CASE("getInvalidTxListWithErrors returns no duplicates") {
  Config cfg(getTestConfig());
  VirtualClock clock;
  Application::pointer app = createTestApplication(clock, cfg);

  auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);
  auto root = app->getRoot();

  // Create accounts for tx sources and fee source
  auto account1 = root->create("a1", minBalance2);
  auto account2 = root->create("a2", minBalance2);
  auto account3 = root->create("a3", minBalance2);
  auto account4 = root->create("a4", minBalance2);

  CheckValidLedgerViewWrapper ledgerView(*app);
  auto balanceOfFeeSource = getAvailableBalance(
      ledgerView.getLedgerHeader().current(),
      ledgerView.getAccount(account2.getPublicKey()).current());

  // Create three fee bumps from account2 (fee source):
  // - fb1: fails checkValid (bad sequence number)
  // - fb2: passes checkValid
  // - fb3: passes checkValid
  // Combined fees of fb2 + fb3 exceed balance, so both should be invalid
  // fb1 is invalid due to checkValid failure
  // This tests that fb1 doesn't appear twice (once from checkValid fail,
  // once from fee check)
  int64_t fee1 = 200;
  int64_t fee2 = balanceOfFeeSource / 2 + 100;
  int64_t fee3 = balanceOfFeeSource / 2 + 100;

  // fb1: Bad seqNum to ensure it fails checkValid
  auto tx1 = transactionFromOperations(
      *app, account1, 555, {payment(account1.getPublicKey(), 1)}, 100);
  auto fb1 = feeBump(*app, account2, tx1, fee1);

  // fb2 and fb3: Valid transactions
  auto tx2 = transactionFromOperations(
      *app, account3, account3.getLastSequenceNumber() + 1,
      {payment(account3.getPublicKey(), 1)}, 100);
  auto fb2 = feeBump(*app, account2, tx2, fee2);

  auto tx3 = transactionFromOperations(
      *app, account4, account4.getLastSequenceNumber() + 1,
      {payment(account4.getPublicKey(), 1)}, 100);
  auto fb3 = feeBump(*app, account2, tx3, fee3);

  // Verify fb1 fails checkValid - inner tx has bad sequence number
  auto diagnostics = DiagnosticEventManager::createDisabled();
  REQUIRE(
      fb1->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0, diagnostics)
          ->getResultCode() == txFEE_BUMP_INNER_FAILED);
  // Verify fb2 and fb3 pass checkValid individually
  REQUIRE(
      fb2->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0, diagnostics)
          ->isSuccess());
  REQUIRE(
      fb3->checkValid(app->getAppConnector(), ledgerView, 0, 0, 0, diagnostics)
          ->isSuccess());

  // Verify combined fees of fb2 + fb3 exceed balance
  REQUIRE(fb2->getFullFee() + fb3->getFullFee() > balanceOfFeeSource);
  // But each individual fee is payable
  REQUIRE(fb2->getFullFee() < balanceOfFeeSource);
  REQUIRE(fb3->getFullFee() < balanceOfFeeSource);

  TxFrameList txs = {fb1, fb2, fb3};
  UnorderedMap<AccountID, int64_t> accountFeeMap;
  auto invalidTxs =
      TxSetUtils::getInvalidTxListWithErrors(txs, *app, accountFeeMap, 0, 0)
          .first;

  // Check for no duplicates by comparing size with unique count
  std::unordered_set<Hash> uniqueHashes;
  for (auto const &tx : invalidTxs) {
    uniqueHashes.insert(tx->getFullHash());
  }
  REQUIRE(invalidTxs.size() == uniqueHashes.size());

  // All 3 txs should be invalid:
  // - fb1: fails checkValid
  // - fb2, fb3: can't pay combined fees
  REQUIRE(invalidTxs.size() == 3);
}

TEST_CASE("txset", "[herder][txset]") {
  SECTION("generalized tx set protocol") {
    uint32_t generalizedTxSetProtocolVersion =
        static_cast<uint32>(SOROBAN_PROTOCOL_VERSION);
#ifdef ENABLE_FASTDEV_UNSAFE_FOR_PRODUCTION
    // Fastdev only links recent Soroban hosts, and this test just needs a
    // generalized-txset-capable protocol.
    generalizedTxSetProtocolVersion =
        Config::CURRENT_LEDGER_PROTOCOL_VERSION - 1;
#endif
    testTxSet(generalizedTxSetProtocolVersion);
  }
  SECTION("protocol current") {
    testTxSet(Config::CURRENT_LEDGER_PROTOCOL_VERSION);
    testTxSetWithFeeBumps(Config::CURRENT_LEDGER_PROTOCOL_VERSION);
  }
}

TEST_CASE("txset with PreconditionsV2", "[herder][txset]") {
  Config cfg(getTestConfig());
  VirtualClock clock;
  Application::pointer app = createTestApplication(clock, cfg);

  auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);
  auto root = app->getRoot();
  auto a1 = root->create("a1", minBalance2);
  auto a2 = root->create("a2", minBalance2);

  // Move close time past 0
  closeLedgerOn(*app, 1, 1, 2022);

  SECTION("minSeqAge") {
    auto minSeqAgeCond = [](Duration minSeqAge) {
      PreconditionsV2 cond;
      cond.minSeqAge = minSeqAge;
      return cond;
    };

    auto test = [&](bool v3ExtIsSet, bool minSeqNumTxIsFeeBump) {
      Duration minGap;
      if (v3ExtIsSet) {
        // run a v19 op so a1's seqLedger is set
        a1.bumpSequence(0);
        closeLedgerOn(*app,
                      app->getLedgerManager().getLastClosedLedgerNum() + 1,
                      app->getLedgerManager()
                              .getLastClosedLedgerHeader()
                              .header.scpValue.closeTime +
                          1);
        minGap = 1;
      } else {
        minGap = app->getLedgerManager()
                     .getLastClosedLedgerHeader()
                     .header.scpValue.closeTime;
      }

      auto txInvalid = transactionWithV2Precondition(*app, a1, 1, 100,
                                                     minSeqAgeCond(minGap + 1));
      TxFrameList removed;
      auto txSet =
          makeTxSetFromTransactions({txInvalid}, *app, 0, 0, removed).second;
      REQUIRE(removed.back() == txInvalid);
      REQUIRE(txSet->sizeTxTotal() == 0);

      // Validate block with invalid tx fails
      {
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto invalidTxSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(100, TxFrameList{txInvalid})}, {}}, *app,
                ledgerHash)
                .second;
        REQUIRE(invalidTxSet->checkValidWithResult(*app, 0, 0) ==
                TxSetValidationResult::TX_VALIDATION_FAILED);
      }

      auto tx1 = transactionWithV2Precondition(*app, a1, 1, 100,
                                               minSeqAgeCond(minGap));

      // only the first tx can have minSeqAge set
      auto tx2Invalid = transactionWithV2Precondition(*app, a2, 2, 100,
                                                      minSeqAgeCond(minGap));

      auto fb1 = feeBump(*app, a1, tx1, 200);
      auto fb2Invalid = feeBump(*app, a2, tx2Invalid, 200);

      removed.clear();
      if (minSeqNumTxIsFeeBump) {
        txSet =
            makeTxSetFromTransactions({fb1, fb2Invalid}, *app, 0, 0, removed)
                .second;
      } else {
        txSet =
            makeTxSetFromTransactions({tx1, tx2Invalid}, *app, 0, 0, removed)
                .second;
      }

      REQUIRE(removed.size() == 1);
      REQUIRE(removed.back() ==
              (minSeqNumTxIsFeeBump ? fb2Invalid : tx2Invalid));

      REQUIRE(txSet->checkValid(*app, 0, 0));

      // Validate block with second invalid tx fails
      {
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto invalidTx = minSeqNumTxIsFeeBump ? fb2Invalid : tx2Invalid;
        auto validTx = minSeqNumTxIsFeeBump ? fb1 : tx1;
        auto invalidTxSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(100, TxFrameList{validTx, invalidTx})}, {}},
                *app, ledgerHash)
                .second;
        REQUIRE(invalidTxSet->checkValidWithResult(*app, 0, 0) ==
                TxSetValidationResult::TX_VALIDATION_FAILED);
      }
    };
    SECTION("before v3 ext is set") { test(false, false); }
    SECTION("after v3 ext is set") { test(true, false); }
    SECTION("after v3 ext is set - fee bump") { test(true, true); }
  }
  SECTION("ledgerBounds") {
    auto ledgerBoundsCond = [](uint32_t minLedger, uint32_t maxLedger) {
      LedgerBounds bounds;
      bounds.minLedger = minLedger;
      bounds.maxLedger = maxLedger;

      PreconditionsV2 cond;
      cond.ledgerBounds.activate() = bounds;
      return cond;
    };

    auto lclNum = app->getLedgerManager().getLastClosedLedgerNum();

    auto tx1 = transaction(*app, a1, 1, 1, 100);

    SECTION("minLedger") {
      auto txInvalid = transactionWithV2Precondition(
          *app, a2, 1, 100, ledgerBoundsCond(lclNum + 2, 0));
      SECTION("build block") {
        TxFrameList removed;
        auto txSet =
            makeTxSetFromTransactions({tx1, txInvalid}, *app, 0, 0, removed);
        REQUIRE(removed.back() == txInvalid);
      }
      SECTION("validate block") {
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto invalidTxSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(100, TxFrameList{tx1, txInvalid})}, {}}, *app,
                ledgerHash)
                .second;
        REQUIRE(invalidTxSet->checkValidWithResult(*app, 0, 0) ==
                TxSetValidationResult::TX_VALIDATION_FAILED);
      }

      // the highest minLedger can be is lcl + 1 because
      // validation is done against the next ledger
      auto tx2 = transactionWithV2Precondition(*app, a2, 1, 100,
                                               ledgerBoundsCond(lclNum + 1, 0));
      TxFrameList removed;
      auto txSet = makeTxSetFromTransactions({tx1, tx2}, *app, 0, 0, removed);
      REQUIRE(removed.empty());
    }
    SECTION("maxLedger") {
      auto txInvalid = transactionWithV2Precondition(
          *app, a2, 1, 100, ledgerBoundsCond(0, lclNum));
      SECTION("build block") {
        TxFrameList removed;
        auto txSet =
            makeTxSetFromTransactions({tx1, txInvalid}, *app, 0, 0, removed);
        REQUIRE(removed.back() == txInvalid);
      }
      SECTION("validate block") {
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto invalidTxSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(100, TxFrameList{tx1, txInvalid})}, {}}, *app,
                ledgerHash)
                .second;
        REQUIRE(invalidTxSet->checkValidWithResult(*app, 0, 0) ==
                TxSetValidationResult::TX_VALIDATION_FAILED);
      }

      // the lower maxLedger can be is lcl + 2, as the current
      // ledger is lcl + 1 and maxLedger bound is exclusive.
      auto tx2 = transactionWithV2Precondition(*app, a2, 1, 100,
                                               ledgerBoundsCond(0, lclNum + 2));
      TxFrameList removed;
      auto txSet = makeTxSetFromTransactions({tx1, tx2}, *app, 0, 0, removed);
      REQUIRE(removed.empty());
    }
  }
  SECTION("extraSigners") {
    SignerKey rootSigner;
    rootSigner.type(SIGNER_KEY_TYPE_ED25519);
    rootSigner.ed25519() = root->getPublicKey().ed25519();

    PreconditionsV2 cond;
    cond.extraSigners.emplace_back(rootSigner);

    SECTION("one extra signer") {
      auto tx = transactionWithV2Precondition(*app, a1, 1, 100, cond);
      SECTION("success") {
        tx->addSignature(root->getSecretKey());
        TxFrameList removed;
        auto txSet = makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
        REQUIRE(removed.empty());
      }
      SECTION("fail") {
        SECTION("build block") {
          TxFrameList removed;
          auto txSet = makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
          REQUIRE(removed.back() == tx);
        }
        SECTION("validate block") {
          auto ledgerHash =
              app->getLedgerManager().getLastClosedLedgerHeader().hash;
          auto invalidTxSet = testtxset::makeNonValidatedGeneralizedTxSet(
                                  {{std::make_pair(100, TxFrameList{tx})}, {}},
                                  *app, ledgerHash)
                                  .second;
          REQUIRE(invalidTxSet->checkValidWithResult(*app, 0, 0) ==
                  TxSetValidationResult::TX_VALIDATION_FAILED);
        }
      }
    }
    SECTION("two extra signers") {
      SignerKey a2Signer;
      a2Signer.type(SIGNER_KEY_TYPE_ED25519);
      a2Signer.ed25519() = a2.getPublicKey().ed25519();

      cond.extraSigners.emplace_back(a2Signer);
      auto tx = transactionWithV2Precondition(*app, a1, 1, 100, cond);
      tx->addSignature(root->getSecretKey());

      SECTION("success") {
        tx->addSignature(a2.getSecretKey());
        TxFrameList removed;
        auto txSet = makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
        REQUIRE(removed.empty());
      }
      SECTION("fail") {
        SECTION("build block") {
          TxFrameList removed;
          auto txSet = makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
          REQUIRE(removed.back() == tx);
        }
        SECTION("validate block") {
          auto ledgerHash =
              app->getLedgerManager().getLastClosedLedgerHeader().hash;
          auto invalidTxSet = testtxset::makeNonValidatedGeneralizedTxSet(
                                  {{std::make_pair(100, TxFrameList{tx})}, {}},
                                  *app, ledgerHash)
                                  .second;
          REQUIRE(invalidTxSet->checkValidWithResult(*app, 0, 0) ==
                  TxSetValidationResult::TX_VALIDATION_FAILED);
        }
      }
    }
    SECTION("duplicate extra signers") {
      cond.extraSigners.emplace_back(rootSigner);
      auto txDupeSigner = transactionWithV2Precondition(*app, a1, 1, 100, cond);
      txDupeSigner->addSignature(root->getSecretKey());
      SECTION("build block") {
        TxFrameList removed;
        auto txSet =
            makeTxSetFromTransactions({txDupeSigner}, *app, 0, 0, removed);
        REQUIRE(removed.back() == txDupeSigner);
        REQUIRE(txDupeSigner->getResultCode() == txMALFORMED);
      }
      SECTION("validate block") {
        auto ledgerHash =
            app->getLedgerManager().getLastClosedLedgerHeader().hash;
        auto invalidTxSet =
            testtxset::makeNonValidatedGeneralizedTxSet(
                {{std::make_pair(100, TxFrameList{txDupeSigner})}, {}}, *app,
                ledgerHash)
                .second;
        REQUIRE(invalidTxSet->checkValidWithResult(*app, 0, 0) ==
                TxSetValidationResult::TX_VALIDATION_FAILED);
      }
    }
    SECTION("signer overlap with default account signer") {
      auto rootTx = transactionWithV2Precondition(*app, *root, 1, 100, cond);
      TxFrameList removed;
      auto txSet = makeTxSetFromTransactions({rootTx}, *app, 0, 0, removed);
      REQUIRE(removed.empty());
    }
    SECTION("signer overlap with added account signer") {
      auto sk1 = makeSigner(*root, 100);
      a1.setOptions(setSigner(sk1));

      auto tx = transactionWithV2Precondition(*app, a1, 1, 100, cond);
      SECTION("signature present") {
        tx->addSignature(root->getSecretKey());

        TxFrameList removed;
        auto txSet = makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
        REQUIRE(removed.empty());
      }
      SECTION("signature missing") {
        SECTION("build block") {
          TxFrameList removed;
          auto txSet = makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
          REQUIRE(removed.back() == tx);
        }
        SECTION("validate block") {
          auto ledgerHash =
              app->getLedgerManager().getLastClosedLedgerHeader().hash;
          auto invalidTxSet = testtxset::makeNonValidatedGeneralizedTxSet(
                                  {{std::make_pair(100, TxFrameList{tx})}, {}},
                                  *app, ledgerHash)
                                  .second;
          REQUIRE(invalidTxSet->checkValidWithResult(*app, 0, 0) ==
                  TxSetValidationResult::TX_VALIDATION_FAILED);
        }
      }
    }
    SECTION("signer overlap with added account signer - both "
            "signers used") {
      auto sk1 = makeSigner(*root, 100);
      a1.setOptions(setSigner(sk1));

      auto tx = transactionFrameFromOps(
          app->getNetworkID(), a1, {root->op(payment(a1, 1))}, {*root}, cond);

      TxFrameList removed;
      auto txSet = makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
      REQUIRE(removed.empty());
    }
  }
}

TEST_CASE("txset base fee", "[herder][txset]") {
  Config cfg(getTestConfig());
  uint32_t const maxTxSetSize = 112;
  cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = maxTxSetSize;

  auto testBaseFee = [&](uint32_t protocolVersion, uint32 nbTransactions,
                         uint32 extraAccounts, size_t lim, int64_t expLowFee,
                         int64_t expHighFee,
                         uint32_t expNotChargedAccounts = 0) {
    cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
    if (!testutil::isTestApplicationProtocolVersionSupported(cfg)) {
      SUCCEED("Skipping historical Soroban protocol test: requested "
              "protocol is not linked in this build");
      return;
    }
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    LedgerHeader lhCopy;
    {
      LedgerTxn ltx(app->getLedgerTxnRoot());
      lhCopy = ltx.loadHeader().current();
    }

    // set up world
    auto root = app->getRoot();

    int64 startingBalance =
        app->getLedgerManager().getLastMinBalance(0) + 10000000;

    auto accounts = std::vector<TestAccount>{};

    std::vector<TransactionFrameBasePtr> txs;
    for (uint32 i = 0; i < nbTransactions; i++) {
      std::string nameI = fmt::format("Base{}", i);
      auto aI = root->create(nameI, startingBalance);
      accounts.push_back(aI);

      auto tx = makeMultiPayment(aI, aI, 1, 1000, 0, 10);
      txs.push_back(tx);
    }

    for (uint32 k = 1; k <= extraAccounts; k++) {
      std::string nameI = fmt::format("Extra{}", k);
      auto aI = root->create(nameI, startingBalance);
      accounts.push_back(aI);

      auto tx = makeMultiPayment(aI, aI, 2, 1000, k, 100);
      txs.push_back(tx);
    }
    auto [txSet, applicableTxSet] = makeTxSetFromTransactions(txs, *app, 0, 0);
    REQUIRE(applicableTxSet->size(lhCopy) == lim);
    REQUIRE(extraAccounts >= 2);

    // fetch balances
    auto getBalances = [&]() {
      std::vector<int64_t> balances;
      std::transform(accounts.begin(), accounts.end(),
                     std::back_inserter(balances),
                     [](TestAccount &a) { return a.getBalance(); });
      return balances;
    };
    auto balancesBefore = getBalances();

    // apply this
    closeLedger(*app, txSet);

    auto balancesAfter = getBalances();
    int64_t lowFee = INT64_MAX, highFee = 0;
    uint32_t notChargedAccounts = 0;
    for (size_t i = 0; i < balancesAfter.size(); i++) {
      auto b = balancesBefore[i];
      auto a = balancesAfter[i];
      auto fee = b - a;
      if (fee == 0) {
        ++notChargedAccounts;
        continue;
      }
      lowFee = std::min(lowFee, fee);
      highFee = std::max(highFee, fee);
    }

    REQUIRE(lowFee == expLowFee);
    REQUIRE(highFee == expHighFee);
    REQUIRE(notChargedAccounts == expNotChargedAccounts);
  };

  // 8 base transactions
  //   1 op, fee bid = baseFee*10 = 1000
  // extra tx
  //   2 ops, fee bid = 20000+i
  //    should add 52 tx (104 ops)

  //  surge threshold is 112-100=12 ops
  //     surge pricing @ 12 (2 extra tx)

  uint32 const baseCount = 8;
  uint32 const extraTx = 52;
  uint32 const newCount = 56; // 112/2
  SECTION("surged") {
    SECTION("mixed") {
      SECTION("generalized tx set protocol") {
        SECTION("fitting exactly into capacity does not cause surge") {
          testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION),
                      baseCount, extraTx, maxTxSetSize, 100, 200);
        }
        SECTION("evicting one tx causes surge") {
          testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION),
                      baseCount + 1, extraTx, maxTxSetSize, 1000, 2000, 1);
        }
      }
      SECTION("protocol current") {
        if (protocolVersionStartsFrom(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                      SOROBAN_PROTOCOL_VERSION)) {
          SECTION("fitting exactly into capacity does not cause surge") {
            testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION),
                        baseCount, extraTx, maxTxSetSize, 100, 200);
          }
          SECTION("evicting one tx causes surge") {
            testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION),
                        baseCount + 1, extraTx, maxTxSetSize, 1000, 2000, 1);
          }
        } else {
          SECTION("maxed out surged") {
            testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION) - 1,
                        baseCount, extraTx, maxTxSetSize, 1000, 2000);
          }
          SECTION("smallest surged") {
            testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION) - 1,
                        baseCount + 1, extraTx - 50, maxTxSetSize - 100 + 1,
                        1000, 2000);
          }
        }
      }
    }
    SECTION("newOnly") {
      SECTION("generalized tx set protocol") {
        SECTION("fitting exactly into capacity does not cause surge") {
          testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION), 0,
                      newCount, maxTxSetSize, 200, 200);
        }
        SECTION("evicting one tx causes surge") {
          testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION), 0,
                      newCount + 1, maxTxSetSize, 20002, 20002, 1);
        }
      }
      SECTION("protocol current") {
        if (protocolVersionStartsFrom(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                      SOROBAN_PROTOCOL_VERSION)) {
          SECTION("fitting exactly into capacity does not cause surge") {
            testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 0, newCount,
                        maxTxSetSize, 200, 200);
          }
          SECTION("evicting one tx causes surge") {
            testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 0,
                        newCount + 1, maxTxSetSize, 20002, 20002, 1);
          }
        } else {
          testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION) - 1, 0,
                      newCount, maxTxSetSize, 20001, 20002);
        }
      }
    }
  }
  SECTION("not surged") {
    SECTION("mixed") {
      SECTION("protocol current") {
        // baseFee = minFee = 100
        // high = 2*minFee
        // highest number of ops not surged is max-100
        testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, baseCount,
                    extraTx - 50, maxTxSetSize - 100, 100, 200);
      }
    }
    SECTION("newOnly") {
      SECTION("protocol current") {
        // low = minFee = 100
        // high = 2*minFee
        // highest number of ops not surged is max-100
        testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 0, newCount - 50,
                    maxTxSetSize - 100, 200, 200);
      }
    }
  }
}

TEST_CASE("tx set hits overlay byte limit during construction",
          "[transactionqueue][soroban]") {
  Config cfg(getTestConfig());
  cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
      Config::CURRENT_LEDGER_PROTOCOL_VERSION;
  auto max = std::numeric_limits<uint32_t>::max();
  cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = max;
  // Pre-create enough genesis accounts for the test
  cfg.GENESIS_TEST_ACCOUNT_COUNT = 100000;

  VirtualClock clock;
  Application::pointer app = createTestApplication(clock, cfg);
  auto root = app->getRoot();

  modifySorobanNetworkConfig(*app, [max](SorobanNetworkConfig &cfg) {
    cfg.mLedgerMaxTxCount = max;
    cfg.mLedgerMaxDiskReadEntries = max;
    cfg.mLedgerMaxDiskReadBytes = max;
    cfg.mLedgerMaxWriteLedgerEntries = max;
    cfg.mLedgerMaxWriteBytes = max;
    cfg.mLedgerMaxTransactionsSizeBytes = max;
    cfg.mLedgerMaxInstructions = max;
  });

  auto conf = [&app]() {
    return app->getLedgerManager().getLastClosedSorobanNetworkConfig();
  };

  uint32_t maxContractSize = 0;
  maxContractSize = conf().maxContractSizeBytes();

  auto makeTx = [&](TestAccount &acc, TxSetPhase const &phase) {
    if (phase == TxSetPhase::SOROBAN) {
      SorobanResources res;
      res.instructions = 1;
      res.diskReadBytes = 0;
      res.writeBytes = 0;

      return createUploadWasmTx(*app, acc, 100, DEFAULT_TEST_RESOURCE_FEE * 10,
                                res, std::nullopt, 0, maxContractSize);
    } else {
      return makeMultiPayment(acc, acc, 100, 1, 100, 1);
    }
  };

  auto testPhaseWithOverlayLimit = [&](TxSetPhase const &phase) {
    TxFrameList txs;
    size_t totalSize = 0;
    int txCount = 0;

    while (totalSize < MAX_TX_SET_ALLOWANCE) {
      auto a = txtest::getGenesisAccount(*app, txCount++);
      txs.emplace_back(makeTx(a, phase));
      totalSize += xdr::xdr_size(txs.back()->getEnvelope());
    }

    PerPhaseTransactionList invalidPhases;
    invalidPhases.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));

    PerPhaseTransactionList phases;
    if (phase == TxSetPhase::SOROBAN) {
      phases = PerPhaseTransactionList{{}, txs};
    } else {
      phases = PerPhaseTransactionList{txs, {}};
    }

    auto [txSet, applicableTxSet] =
        makeTxSetFromTransactions(phases, *app, 0, 0, invalidPhases);
    REQUIRE(txSet->encodedSize() <= MAX_TX_SET_ALLOWANCE);

    REQUIRE(invalidPhases[static_cast<size_t>(phase)].empty());
    auto const &phaseTxs = applicableTxSet->getPhase(phase);
    auto trimmedSize =
        std::accumulate(phaseTxs.begin(), phaseTxs.end(), size_t(0),
                        [&](size_t a, TransactionFrameBasePtr const &tx) {
                          return a += xdr::xdr_size(tx->getEnvelope());
                        });

    auto byteAllowance = phase == TxSetPhase::SOROBAN
                             ? app->getConfig().getSorobanByteAllowance()
                             : app->getConfig().getClassicByteAllowance();
    REQUIRE(trimmedSize > byteAllowance - conf().txMaxSizeBytes());
    REQUIRE(trimmedSize <= byteAllowance);
  };

  SECTION("soroban") { testPhaseWithOverlayLimit(TxSetPhase::SOROBAN); }
  SECTION("classic") { testPhaseWithOverlayLimit(TxSetPhase::CLASSIC); }
}

TEST_CASE("surge pricing", "[herder][txset][soroban]") {
  SECTION("max 0 ops per ledger") {
    Config cfg(getTestConfig(0, Config::TESTDB_IN_MEMORY));

    SECTION("classic") {
      cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 0;

      VirtualClock clock;
      Application::pointer app = createTestApplication(clock, cfg);
      auto root = app->getRoot();

      auto destAccount = root->create("destAccount", 500000000);

      auto tx = makeMultiPayment(destAccount, *root, 1, 100, 0, 1);

      TxFrameList invalidTxs;
      auto txSet =
          makeTxSetFromTransactions({tx}, *app, 0, 0, invalidTxs).second;

      // Transaction is valid, but trimmed by surge pricing.
      REQUIRE(invalidTxs.empty());
      REQUIRE(txSet->sizeTxTotal() == 0);
    }
    SECTION("soroban") {
      // Dont set TESTING_UPGRADE_MAX_TX_SET_SIZE for soroban test case
      // because we need to submit a TX for the actual kill switch
      // upgrade.
      VirtualClock clock;
      Application::pointer app = createTestApplication(clock, cfg);
      auto root = app->getRoot();

      auto destAccount = root->create("destAccount", 500000000);

      uint32_t const baseFee = 10'000'000;
      modifySorobanNetworkConfig(
          *app, [](SorobanNetworkConfig &cfg) { cfg.mLedgerMaxTxCount = 0; });
      SorobanResources resources;
      auto sorobanTx = createUploadWasmTx(*app, *root, baseFee,
                                          DEFAULT_TEST_RESOURCE_FEE, resources);

      PerPhaseTransactionList invalidTxs;
      invalidTxs.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
      auto txSet =
          makeTxSetFromTransactions(PerPhaseTransactionList{{}, {sorobanTx}},
                                    *app, 0, 0, invalidTxs)
              .second;

      // Transaction is valid, but trimmed by surge pricing.
      REQUIRE(std::all_of(invalidTxs.begin(), invalidTxs.end(),
                          [](auto const &txs) { return txs.empty(); }));
      REQUIRE(txSet->sizeTxTotal() == 0);
    }
  }
  SECTION("soroban txs") {
    Config cfg(getTestConfig());
    // Max 1 classic op
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1;

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);
    // Max 2 soroban ops
    modifySorobanNetworkConfig(
        *app, [](SorobanNetworkConfig &cfg) { cfg.mLedgerMaxTxCount = 2; });

    auto root = app->getRoot();
    auto acc1 = root->create("account1", 500000000);
    auto acc2 = root->create("account2", 500000000);
    auto acc3 = root->create("account3", 500000000);
    auto acc4 = root->create("account4", 500000000);
    auto acc5 = root->create("account5", 500000000);
    auto acc6 = root->create("account6", 500000000);

    // Ensure these accounts don't overlap with classic tx (with root source
    // account)
    std::vector<TestAccount> accounts = {acc1, acc2, acc3, acc4, acc5, acc6};

    // Valid classic
    auto tx = makeMultiPayment(acc1, *root, 1, 100, 0, 1);

    SorobanNetworkConfig conf =
        app->getLedgerManager().getLastClosedSorobanNetworkConfig();

    uint32_t const baseFee = 10'000'000;
    SorobanResources resources;
    resources.instructions = 800'000;
    resources.diskReadBytes = conf.txMaxDiskReadBytes();
    resources.writeBytes = 1000;
    auto sorobanTx = createUploadWasmTx(*app, acc2, baseFee,
                                        DEFAULT_TEST_RESOURCE_FEE, resources);

    auto generateTxs = [&](std::vector<TestAccount> &accounts,
                           SorobanNetworkConfig conf) {
      TxFrameList txs;
      for (auto &acc : accounts) {
        SorobanResources res;
        res.instructions = rand_uniform<uint32_t>(
            1, static_cast<uint32>(conf.txMaxInstructions()));
        res.diskReadBytes =
            rand_uniform<uint32_t>(1, conf.txMaxDiskReadBytes());
        res.writeBytes = rand_uniform<uint32_t>(1, conf.txMaxWriteBytes());
        auto read = rand_uniform<uint32_t>(0, conf.txMaxDiskReadEntries());
        auto write = rand_uniform<uint32_t>(
            0, std::min(conf.txMaxWriteLedgerEntries(),
                        (conf.txMaxDiskReadEntries() - read)));
        for (auto const &key :
             LedgerTestUtils::generateUniqueValidSorobanLedgerEntryKeys(
                 write)) {
          res.footprint.readWrite.emplace_back(key);
        }
        for (auto const &key :
             LedgerTestUtils::generateUniqueValidSorobanLedgerEntryKeys(read)) {
          res.footprint.readOnly.emplace_back(key);
        }

        auto tx = createUploadWasmTx(*app, acc, baseFee * 10,
                                     /* refundableFee */ baseFee, res);
        if (rand_flip()) {
          txs.emplace_back(tx);
        } else {
          // Double the inclusion fee
          txs.emplace_back(feeBump(*app, acc, tx, baseFee * 10 * 2));
        }
        CLOG_INFO(Herder,
                  "Generated tx with {} instructions, {} read "
                  "bytes, {} write bytes, data bytes, {} read "
                  "ledger entries, {} write ledger entries",
                  res.instructions, res.diskReadBytes, res.writeBytes, read,
                  write);
      }
      return txs;
    };

    SECTION("invalid soroban is rejected") {
      SECTION("invalid fee") {
        // Fee too small
        auto invalidSoroban = createUploadWasmTx(
            *app, acc2, 100, DEFAULT_TEST_RESOURCE_FEE, resources);

        SECTION("build block") {
          PerPhaseTransactionList invalidPhases;
          invalidPhases.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
          auto txSet = makeTxSetFromTransactions(
                           PerPhaseTransactionList{{tx}, {invalidSoroban}},
                           *app, 0, 0, invalidPhases)
                           .second;

          // Soroban tx is rejected
          REQUIRE(txSet->sizeTxTotal() == 1);
          REQUIRE(invalidPhases[0].empty());
          REQUIRE(invalidPhases[1].size() == 1);
          REQUIRE(invalidPhases[1][0]->getFullHash() ==
                  invalidSoroban->getFullHash());
        }
        SECTION("validate block") {
          auto ledgerHash =
              app->getLedgerManager().getLastClosedLedgerHeader().hash;
          auto invalidTxSet =
              testtxset::makeNonValidatedGeneralizedTxSet(
                  {{std::make_pair(std::nullopt, TxFrameList{tx})},
                   {std::make_pair(baseFee, TxFrameList{invalidSoroban})}},
                  *app, ledgerHash)
                  .second;
          REQUIRE(invalidTxSet->checkValidWithResult(*app, 0, 0) ==
                  TxSetValidationResult::TX_FEE_BID_TOO_LOW);
        }
      }
      SECTION("invalid resource, multiple transactions, resources exceed "
              "ledger limit") {
        // Create two txs that are individually valid but combined
        // exceed ledger resource limits
        // Each tx uses 60% of ledger max, so combined > 100%
        auto insns =
            static_cast<uint32_t>(conf.ledgerMaxInstructions() * 6 / 10);
        resources.instructions = insns;
        auto soroban1 = createUploadWasmTx(
            *app, acc2, baseFee, DEFAULT_TEST_RESOURCE_FEE, resources);
        // Pick soroban2 by fee
        auto soroban2 = createUploadWasmTx(
            *app, acc3, baseFee + 1, DEFAULT_TEST_RESOURCE_FEE, resources);

        SECTION("build block") {
          // When building, one tx should be trimmed due to limit
          PerPhaseTransactionList invalidPhases;
          invalidPhases.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
          auto txSet = makeTxSetFromTransactions(
                           PerPhaseTransactionList{{tx}, {soroban1, soroban2}},
                           *app, 0, 0, invalidPhases)
                           .second;
          // Both txs are valid individually, but only one fits
          REQUIRE(txSet->sizeTxTotal() == 2);
          REQUIRE(invalidPhases[0].empty());
          REQUIRE(invalidPhases[1].size() == 1);
          REQUIRE(invalidPhases[1][0]->getFullHash() ==
                  soroban1->getFullHash());
        }
        SECTION("validate block") {
          // When validating a received block with both txs, it
          // should fail due to exceeding resource limits
          auto ledgerHash =
              app->getLedgerManager().getLastClosedLedgerHeader().hash;
          auto invalidTxSet =
              testtxset::makeNonValidatedGeneralizedTxSet(
                  {{std::make_pair(std::nullopt, TxFrameList{tx})},
                   {std::make_pair(std::nullopt,
                                   TxFrameList{soroban1, soroban2})}},
                  *app, ledgerHash)
                  .second;
          REQUIRE(invalidTxSet->checkValidWithResult(*app, 0, 0) ==
                  TxSetValidationResult::SOROBAN_RESOURCES_EXCEED_LIMIT);
        }
      }
    }
    SECTION("classic and soroban fit") {
      PerPhaseTransactionList invalidPhases;
      invalidPhases.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
      auto txSet =
          makeTxSetFromTransactions(PerPhaseTransactionList{{tx}, {sorobanTx}},
                                    *app, 0, 0, invalidPhases)
              .second;

      // Everything fits
      REQUIRE(std::all_of(invalidPhases.begin(), invalidPhases.end(),
                          [](auto const &txs) { return txs.empty(); }));
      REQUIRE(txSet->sizeTxTotal() == 2);
    }
    SECTION("classic and soroban in the same phase are rejected") {
      PerPhaseTransactionList invalidPhases;
      invalidPhases.resize(1);
      REQUIRE_THROWS_AS(
          makeTxSetFromTransactions(PerPhaseTransactionList{{tx, sorobanTx}},
                                    *app, 0, 0, invalidPhases),
          std::runtime_error);
    }
    SECTION("soroban surge pricing, classic unaffected") {
      // Another soroban tx with higher fee, which will be selected
      auto sorobanTxHighFee = createUploadWasmTx(
          *app, acc3, baseFee * 2, DEFAULT_TEST_RESOURCE_FEE, resources);
      PerPhaseTransactionList invalidPhases;
      invalidPhases.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
      auto txSet =
          makeTxSetFromTransactions(
              PerPhaseTransactionList{{tx}, {sorobanTx, sorobanTxHighFee}},
              *app, 0, 0, invalidPhases)
              .second;

      REQUIRE(std::all_of(invalidPhases.begin(), invalidPhases.end(),
                          [](auto const &txs) { return txs.empty(); }));
      REQUIRE(txSet->sizeTxTotal() == 2);
      auto const &classicPhase = txSet->getPhase(TxSetPhase::CLASSIC);
      REQUIRE(classicPhase.sizeTx() == 1);
      for (auto it = classicPhase.begin(); it != classicPhase.end(); ++it) {
        REQUIRE((*it)->getFullHash() == tx->getFullHash());
      }
      auto const &sorobanPhase = txSet->getPhase(TxSetPhase::SOROBAN);
      REQUIRE(sorobanPhase.sizeTx() == 1);
      for (auto it = sorobanPhase.begin(); it != sorobanPhase.end(); ++it) {
        REQUIRE((*it)->getFullHash() == sorobanTxHighFee->getFullHash());
      }
    }
    SECTION("soroban surge pricing with gap") {
      // Another soroban tx with high fee and a bit less resources
      // Still half capacity available
      resources.diskReadBytes = conf.txMaxDiskReadBytes() / 2;
      auto sorobanTxHighFee = createUploadWasmTx(
          *app, acc3, baseFee * 2, DEFAULT_TEST_RESOURCE_FEE, resources);

      // Create another small soroban tx, with small fee. It should be
      // picked up anyway since we can't fit sorobanTx (gaps are allowed)
      resources.instructions = 1;
      resources.diskReadBytes = 1;
      resources.writeBytes = 1;

      auto smallSorobanLowFee = createUploadWasmTx(
          *app, acc4, baseFee / 10, DEFAULT_TEST_RESOURCE_FEE, resources);

      PerPhaseTransactionList invalidPhases;
      invalidPhases.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
      auto txSet =
          makeTxSetFromTransactions(
              PerPhaseTransactionList{
                  {tx}, {sorobanTxHighFee, smallSorobanLowFee, sorobanTx}},
              *app, 0, 0, invalidPhases)
              .second;

      REQUIRE(std::all_of(invalidPhases.begin(), invalidPhases.end(),
                          [](auto const &txs) { return txs.empty(); }));
      REQUIRE(txSet->sizeTxTotal() == 3);
      auto const &classicTxs =
          txSet->getPhase(TxSetPhase::CLASSIC).getSequentialTxs();
      REQUIRE(classicTxs.size() == 1);
      REQUIRE(classicTxs[0]->getFullHash() == tx->getFullHash());
      for (auto const &t : txSet->getPhase(TxSetPhase::SOROBAN)) {
        // smallSorobanLowFee was picked over sorobanTx to fill the gap
        bool pickedGap = t->getFullHash() == sorobanTxHighFee->getFullHash() ||
                         t->getFullHash() == smallSorobanLowFee->getFullHash();
        REQUIRE(pickedGap);
      }
    }
    SECTION("tx set construction limits") {
      int const ITERATIONS = 20;
      for (int i = 0; i < ITERATIONS; i++) {
        SECTION("iteration " + std::to_string(i)) {
          PerPhaseTransactionList invalidPhases;
          invalidPhases.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
          auto txSet =
              makeTxSetFromTransactions(
                  PerPhaseTransactionList{{tx}, generateTxs(accounts, conf)},
                  *app, 0, 0, invalidPhases)
                  .second;

          REQUIRE(std::all_of(invalidPhases.begin(), invalidPhases.end(),
                              [](auto const &txs) { return txs.empty(); }));
          int count = 0;
          for (auto it = txSet->getPhase(TxSetPhase::CLASSIC).begin();
               it != txSet->getPhase(TxSetPhase::CLASSIC).end(); ++it) {
            REQUIRE((*it)->getFullHash() == tx->getFullHash());
            ++count;
          }
          REQUIRE(count == 1);

          auto sorobanSize = txSet->getPhase(TxSetPhase::SOROBAN).sizeTx();
          // Depending on resources generated for each tx, can only
          // fit 1 or 2 transactions
          bool expectedSorobanTxs = sorobanSize == 1 || sorobanSize == 2;
          REQUIRE(expectedSorobanTxs);
        }
      }
    }
    SECTION("tx sets over limits are invalid") {
      TxFrameList txs = generateTxs(accounts, conf);
      auto ledgerHash =
          app->getLedgerManager().getLastClosedLedgerHeader().hash;
      auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
                       {{}, {std::make_pair(500, txs)}}, *app, ledgerHash)
                       .second;

      REQUIRE(txSet->checkValidWithResult(*app, 0, 0) ==
              TxSetValidationResult::SOROBAN_RESOURCES_EXCEED_LIMIT);
    }
  }
}

TEST_CASE("surge pricing with DEX separation", "[herder][txset]") {
  if (protocolVersionIsBefore(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                              SOROBAN_PROTOCOL_VERSION)) {
    return;
  }
  Config cfg(getTestConfig());
  cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
      Config::CURRENT_LEDGER_PROTOCOL_VERSION;
  cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 15;
  cfg.MAX_DEX_TX_OPERATIONS_IN_TX_SET = 5;

  VirtualClock clock;
  Application::pointer app = createTestApplication(clock, cfg);

  auto root = app->getRoot();

  auto accountA = root->create("accountA", 5000000000);
  auto accountB = root->create("accountB", 5000000000);
  auto accountC = root->create("accountC", 5000000000);
  auto accountD = root->create("accountD", 5000000000);

  auto seqNumA = accountA.getLastSequenceNumber();
  auto seqNumB = accountB.getLastSequenceNumber();
  auto seqNumC = accountC.getLastSequenceNumber();
  auto seqNumD = accountD.getLastSequenceNumber();

  auto runTest = [&](std::vector<TransactionFrameBasePtr> const &txs,
                     size_t expectedTxsA, size_t expectedTxsB,
                     size_t expectedTxsC, size_t expectedTxsD,
                     int64_t expectedNonDexBaseFee,
                     int64_t expectedDexBaseFee) {
    auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0).second;
    size_t cntA = 0, cntB = 0, cntC = 0, cntD = 0;
    auto const &phases = txSet->getPhasesInApplyOrder();

    for (auto const &tx : phases[static_cast<size_t>(TxSetPhase::CLASSIC)]) {
      if (tx->getSourceID() == accountA.getPublicKey()) {
        ++cntA;
        ++seqNumA;
        REQUIRE(seqNumA == tx->getSeqNum());
      }
      if (tx->getSourceID() == accountB.getPublicKey()) {
        ++cntB;
        ++seqNumB;
        REQUIRE(seqNumB == tx->getSeqNum());
      }
      if (tx->getSourceID() == accountC.getPublicKey()) {
        ++cntC;
        ++seqNumC;
        REQUIRE(seqNumC == tx->getSeqNum());
      }
      if (tx->getSourceID() == accountD.getPublicKey()) {
        ++cntD;
        ++seqNumD;
        REQUIRE(seqNumD == tx->getSeqNum());
      }

      auto baseFee = txSet->getTxBaseFee(tx);
      REQUIRE(baseFee);
      if (tx->hasDexOperations()) {
        REQUIRE(*baseFee == expectedDexBaseFee);
      } else {
        REQUIRE(*baseFee == expectedNonDexBaseFee);
      }
    }

    REQUIRE(cntA == expectedTxsA);
    REQUIRE(cntB == expectedTxsB);
    REQUIRE(cntC == expectedTxsC);
    REQUIRE(cntD == expectedTxsD);
  };

  auto nonDexTx = [](TestAccount &account, uint32 nbOps, uint32_t opFee) {
    return makeSelfPayment(account, nbOps, opFee * nbOps);
  };
  auto dexTx = [&](TestAccount &account, uint32 nbOps, uint32_t opFee) {
    return createSimpleDexTx(*app, account, nbOps, opFee * nbOps);
  };
  SECTION("only non-DEX txs") {
    runTest({nonDexTx(accountA, 8, 200), nonDexTx(accountB, 4, 300),
             nonDexTx(accountC, 2, 400),
             /* cutoff */
             nonDexTx(accountD, 2, 100)},
            1, 1, 1, 0, 200, 0);
  }
  SECTION("only DEX txs") {
    runTest({dexTx(accountA, 2, 200), dexTx(accountB, 1, 300),
             dexTx(accountC, 2, 400),
             /* cutoff */
             dexTx(accountD, 1, 100)},
            1, 1, 1, 0, 0, 200);
  }
  SECTION("mixed txs") {
    SECTION("only DEX surge priced") {
      SECTION("DEX limit reached") {
        runTest(
            {
                /* 6 non-DEX ops + 5 DEX ops = 11 ops */
                nonDexTx(accountA, 6, 100),
                dexTx(accountB, 5, 400),
                /* cutoff */
                dexTx(accountC, 1, 200),
                dexTx(accountD, 1, 399),
            },
            1, 1, 0, 0, 100, 400);
      }
      SECTION("both limits reached, but only DEX evicted") {
        runTest(
            {
                /* 10 non-DEX ops + 5 DEX ops = 15 ops */
                nonDexTx(accountA, 10, 100),
                dexTx(accountB, 5, 400),
                /* cutoff */
                dexTx(accountC, 1, 399),
                dexTx(accountD, 1, 399),
            },
            1, 1, 0, 0, 100, 400);
      }
    }
    SECTION("all txs surge priced") {
      SECTION("only global limit reached") {
        runTest(
            {
                /* 13 non-DEX ops + 2 DEX ops = 15 ops */
                nonDexTx(accountA, 13, 250),
                dexTx(accountB, 2, 250),
                /* cutoff */
                dexTx(accountC, 1, 200),
                nonDexTx(accountD, 1, 249),
            },
            1, 1, 0, 0, 250, 250);
      }
      SECTION("both limits reached") {
        SECTION("non-DEX fee is lowest") {
          runTest(
              {
                  /* 10 non-DEX ops + 5 DEX ops = 15 ops */
                  nonDexTx(accountA, 10, 250),
                  dexTx(accountB, 5, 400),
                  /* cutoff */
                  dexTx(accountC, 1, 399),
                  nonDexTx(accountD, 1, 249),
              },
              1, 1, 0, 0, 250, 400);
        }
        SECTION("DEX fee is lowest") {
          runTest(
              {
                  /* 10 non-DEX ops + 5 DEX ops = 15 ops */
                  nonDexTx(accountA, 10, 500),
                  dexTx(accountB, 5, 200),
                  /* cutoff */
                  dexTx(accountC, 1, 199),
                  nonDexTx(accountD, 1, 199),
              },
              1, 1, 0, 0, 200, 200);
        }
      }
    }
  }
}

TEST_CASE("surge pricing with DEX separation holds invariants",
          "[herder][txset]") {
  if (protocolVersionIsBefore(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                              SOROBAN_PROTOCOL_VERSION)) {
    return;
  }

  auto runTest = [](std::optional<uint32_t> maxDexOps, int dexOpsPercent) {
    Config cfg(getTestConfig());
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 20;
    cfg.MAX_DEX_TX_OPERATIONS_IN_TX_SET = maxDexOps;
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    LedgerHeader lhCopy;
    {
      LedgerTxn ltx(app->getLedgerTxnRoot());
      lhCopy = ltx.loadHeader().current();
    }

    uniform_int_distribution<> isDexTxDistr(0, 100);
    uniform_int_distribution<> numOpsDistr(1, 5);
    uniform_int_distribution<> feeDistr(100, 1000);
    uniform_int_distribution<> addFeeDistr(0, 5);
    uniform_int_distribution<> txCountDistr(1, 30);

    auto root = app->getRoot();

    int nextAccId = 1;

    auto genTx = [&]() {
      auto account = root->create(std::to_string(nextAccId), 5000000000);
      ++nextAccId;
      uint32 ops = numOpsDistr(Catch::rng());
      int fee = ops * feeDistr(Catch::rng()) + addFeeDistr(Catch::rng());
      if (isDexTxDistr(Catch::rng()) < dexOpsPercent) {
        return createSimpleDexTx(*app, account, ops, fee);
      } else {
        return makeSelfPayment(account, ops, fee);
      }
    };
    auto genTxs = [&](int cnt) {
      std::vector<TransactionFrameBasePtr> txs;
      for (int i = 0; i < cnt; ++i) {
        txs.emplace_back(genTx());
      }
      return txs;
    };

    for (int iter = 0; iter < 50; ++iter) {
      auto txs = genTxs(txCountDistr(Catch::rng()));
      auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0).second;

      auto const &phases = txSet->getPhasesInApplyOrder();
      std::array<uint32_t, 2> opsCounts{};
      std::array<int64_t, 2> baseFees{};

      for (auto const &resTx :
           phases[static_cast<size_t>(TxSetPhase::CLASSIC)]) {
        auto isDex = static_cast<size_t>(resTx->hasDexOperations());
        opsCounts[isDex] += resTx->getNumOperations();
        auto baseFee = txSet->getTxBaseFee(resTx);
        REQUIRE(baseFee);
        if (baseFees[isDex] != 0) {
          // All base fees should be the same among the
          // transaction categories.
          REQUIRE(baseFees[isDex] == *baseFee);
        } else {
          baseFees[isDex] = *baseFee;
        }
      }

      REQUIRE(opsCounts[0] + opsCounts[1] <=
              cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
      if (maxDexOps) {
        REQUIRE(opsCounts[1] <= *maxDexOps);
      }
      // DEX transaction base fee has to be not smaller than generic
      // transaction base fee.
      if (baseFees[0] > 0 && baseFees[1] > 0) {
        REQUIRE(baseFees[0] <= baseFees[1]);
      }
    }
  };

  SECTION("no DEX limit") { runTest(std::nullopt, 50); }
  SECTION("low DEX limit") {
    SECTION("medium DEX tx fraction") { runTest(5, 50); }
    SECTION("high DEX tx fraction") { runTest(5, 80); }
    SECTION("only DEX txs") { runTest(5, 100); }
  }
  SECTION("high DEX limit") {
    SECTION("medium DEX tx fraction") { runTest(15, 50); }
    SECTION("high DEX tx fraction") { runTest(15, 80); }
    SECTION("only DEX txs") { runTest(15, 100); }
  }
}

TEST_CASE("generalized tx set applied to ledger", "[herder][txset][soroban]") {
  Config cfg(getTestConfig());
  cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;

  VirtualClock clock;
  Application::pointer app = createTestApplication(clock, cfg);
  auto root = app->getRoot();
  overrideSorobanNetworkConfigForTest(*app);
  int64 startingBalance =
      app->getLedgerManager().getLastMinBalance(0) + 10000000;

  std::vector<TestAccount> accounts;
  int txCnt = 0;
  auto addTx = [&](int nbOps, uint32_t fee) {
    auto account = root->create(std::to_string(txCnt++), startingBalance);
    accounts.push_back(account);
    return makeSelfPayment(account, nbOps, fee);
  };

  SorobanResources resources;
  resources.instructions = 3'000'000;
  resources.diskReadBytes = 0;
  resources.writeBytes = 2000;
  auto dummyAccount = root->create("dummy", startingBalance);
  auto dummyUploadTx =
      createUploadWasmTx(*app, dummyAccount, 100, 1000, resources);
  UnorderedSet<LedgerKey> seenKeys;
  auto keys = LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
      {CONTRACT_DATA}, 1, seenKeys);
  resources.footprint.readWrite.push_back(keys.front());
  auto resourceFee = sorobanResourceFee(
      *app, resources, xdr::xdr_size(dummyUploadTx->getEnvelope()), 40);

  uint32_t const rentFee = protocolVersionIsBefore(getLclProtocolVersion(*app),
                                                   ProtocolVersion::V_26)
                               ? 20'368
                               : 20'369;
  resourceFee += rentFee;
  resources.footprint.readWrite.pop_back();
  auto addSorobanTx = [&](uint32_t inclusionFee) {
    auto account = root->create(std::to_string(txCnt++), startingBalance);
    accounts.push_back(account);
    return createUploadWasmTx(*app, account, inclusionFee, resourceFee,
                              resources);
  };

  auto checkFees = [&](std::pair<TxSetXDRFrameConstPtr,
                                 ApplicableTxSetFrameConstPtr> const &txSet,
                       std::vector<int64_t> const &expectedFeeCharged,
                       bool validateTxSet = true) {
    if (validateTxSet) {
      REQUIRE(txSet.second->checkValid(*app, 0, 0));
    }

    auto getBalances = [&]() {
      std::vector<int64_t> balances;
      std::transform(accounts.begin(), accounts.end(),
                     std::back_inserter(balances),
                     [](TestAccount &a) { return a.getBalance(); });
      return balances;
    };
    auto balancesBefore = getBalances();

    auto res = closeLedgerOn(
        *app, app->getLedgerManager().getLastClosedLedgerNum() + 1,
        getTestDate(13, 4, 2022), txSet.first);

    REQUIRE(res.results.size() == txSet.second->sizeTxTotal());
    for (size_t i = 0; i < res.results.size(); ++i) {
      checkTx(i, res, txSUCCESS);
    }

    auto balancesAfter = getBalances();
    std::vector<int64_t> feeCharged;
    for (size_t i = 0; i < balancesAfter.size(); i++) {
      feeCharged.push_back(balancesBefore[i] - balancesAfter[i]);
    }

    REQUIRE(feeCharged == expectedFeeCharged);
  };

  SECTION("single discounted component") {
    auto tx1 = addTx(3, 3500);
    auto tx2 = addTx(2, 5000);
    auto ledgerHash = app->getLedgerManager().getLastClosedLedgerHeader().hash;
    auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
        {{std::make_pair(1000, std::vector<TransactionFrameBasePtr>{tx1, tx2})},
         {}},
        *app, ledgerHash);
    checkFees(txSet, {3000, 2000});
  }
  SECTION("single non-discounted component") {
    auto tx1 = addTx(3, 3500);
    auto tx2 = addTx(2, 5000);
    auto ledgerHash = app->getLedgerManager().getLastClosedLedgerHeader().hash;
    auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
        {{std::make_pair(std::nullopt,
                         std::vector<TransactionFrameBasePtr>{tx1, tx2})},
         {}},
        *app, ledgerHash);
    checkFees(txSet, {3500, 5000});
  }
  SECTION("multiple components") {
    auto tx1 = addTx(3, 3500);
    auto tx2 = addTx(2, 5000);
    auto tx3 = addTx(1, 501);
    auto tx4 = addTx(5, 10000);
    auto tx5 = addTx(4, 15000);
    auto tx6 = addTx(5, 35000);
    auto tx7 = addTx(1, 10000);
    auto ledgerHash = app->getLedgerManager().getLastClosedLedgerHeader().hash;

    std::vector<
        std::pair<std::optional<int64_t>, std::vector<TransactionFrameBasePtr>>>
        components = {
            std::make_pair(1000,
                           std::vector<TransactionFrameBasePtr>{tx1, tx2}),
            std::make_pair(500, std::vector<TransactionFrameBasePtr>{tx3, tx4}),
            std::make_pair(2000, std::vector<TransactionFrameBasePtr>{tx5}),
            std::make_pair(std::nullopt,
                           std::vector<TransactionFrameBasePtr>{tx6, tx7})};
    auto txSet = testtxset::makeNonValidatedGeneralizedTxSet({components, {}},
                                                             *app, ledgerHash);
    checkFees(txSet, {3000, 2000, 500, 2500, 8000, 35000, 10000});
  }
  SECTION("soroban") {
    auto tx1 = addTx(3, 3500);
    auto tx2 = addTx(2, 5000);
    auto sorobanTx1 = addSorobanTx(5000);
    auto sorobanTx2 = addSorobanTx(10000);
    auto ledgerHash = app->getLedgerManager().getLastClosedLedgerHeader().hash;

    auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
        {
            {std::make_pair(1000,
                            std::vector<TransactionFrameBasePtr>{tx1, tx2})},
            {std::make_pair(
                2000,
                std::vector<TransactionFrameBasePtr>{sorobanTx1, sorobanTx2})},
        },
        *app, ledgerHash);
    SECTION("with validation") {
      checkFees(txSet, {3000, 2000, 2000 + resourceFee, 2000 + resourceFee});
    }
    SECTION("without validation") {
      checkFees(txSet, {3000, 2000, 2000 + resourceFee, 2000 + resourceFee},
                /* validateTxSet */ false);
    }
  }
}

static void testSCPDriver(uint32 protocolVersion, uint32_t maxTxSetSize,
                          size_t expectedOps) {
  using SVUpgrades = decltype(StellarValue::upgrades);

  Config cfg(getTestConfig(0, Config::TESTDB_DEFAULT));

  cfg.MANUAL_CLOSE = false;
  cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
  cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
  cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = maxTxSetSize;
  cfg.GENESIS_TEST_ACCOUNT_COUNT = 1000;

  VirtualClock clock;
  auto s = SecretKey::pseudoRandomForTesting();
  cfg.QUORUM_SET.validators.emplace_back(s.getPublicKey());

  Application::pointer app = createTestApplication(clock, cfg);

  auto root = app->getRoot();
  std::vector<TestAccount> accounts;
  for (int i = 0; i < 1000; ++i) {
    auto account = txtest::getGenesisAccount(*app, i);
    accounts.emplace_back(account);
  }

  auto const &lcl = app->getLedgerManager().getLastClosedLedgerHeader();
  using TxPair = std::pair<Value, TxSetXDRFrameConstPtr>;
  auto makeTxUpgradePair = [&](HerderImpl &herder, TxSetXDRFrameConstPtr txSet,
                               uint64_t closeTime, SVUpgrades const &upgrades) {
    StellarValue sv = herder.makeStellarValue(
        txSet->getContentsHash(), closeTime, upgrades, root->getSecretKey());
    auto v = xdr::xdr_to_opaque(sv);
    return TxPair{v, txSet};
  };
  auto makeTxPair = [&](HerderImpl &herder, TxSetXDRFrameConstPtr txSet,
                        uint64_t closeTime) {
    return makeTxUpgradePair(herder, txSet, closeTime, emptyUpgradeSteps);
  };
  auto makeEnvelope = [&s](HerderImpl &herder, TxPair const &p, Hash qSetHash,
                           uint64_t slotIndex, bool nomination) {
    // herder must want the TxSet before receiving it, so we are sending it
    // fake envelope
    auto envelope = SCPEnvelope{};
    envelope.statement.slotIndex = slotIndex;
    if (nomination) {
      envelope.statement.pledges.type(SCP_ST_NOMINATE);
      envelope.statement.pledges.nominate().votes.push_back(p.first);
      envelope.statement.pledges.nominate().quorumSetHash = qSetHash;
    } else {
      envelope.statement.pledges.type(SCP_ST_PREPARE);
      envelope.statement.pledges.prepare().ballot.value = p.first;
      envelope.statement.pledges.prepare().quorumSetHash = qSetHash;
    }
    envelope.statement.nodeID = s.getPublicKey();
    herder.signEnvelope(s, envelope);
    return envelope;
  };
  auto makeTransactions = [&](int n, int nbOps, uint32 feeMulti) {
    std::vector<TransactionFrameBasePtr> txs(n);
    size_t index = 0;

    std::generate(std::begin(txs), std::end(txs), [&]() {
      accounts[index].loadSequenceNumber();
      return makeMultiPayment(*root, accounts[index++], nbOps, 1000, 0,
                              feeMulti);
    });

    return makeTxSetFromTransactions(txs, *app, 0, 0);
  };

  SECTION("combineCandidates") {
    auto &herder = static_cast<HerderImpl &>(app->getHerder());

    ValueWrapperPtrSet candidates;

    auto addToCandidates = [&](TxPair const &p) {
      auto envelope = makeEnvelope(
          herder, p, {}, herder.trackingConsensusLedgerIndex() + 1, true);
      REQUIRE(herder.recvSCPEnvelope(envelope) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(herder.recvTxSet(p.second->getContentsHash(), p.second));
      auto v = herder.getHerderSCPDriver().wrapValue(p.first);
      candidates.emplace(v);
    };

    struct CandidateSpec {
      int const n;
      int const nbOps;
      uint32 const feeMulti;
      TimePoint const closeTime;
      std::optional<uint32> const baseFeeIncrement;
    };

    std::vector<Hash> txSetHashes;
    std::vector<size_t> txSetSizes;
    std::vector<size_t> txSetOpSizes;
    std::vector<TimePoint> closeTimes;
    std::vector<decltype(lcl.header.baseFee)> baseFees;

    auto addCandidateThenTest = [&](CandidateSpec const &spec) {
      // Create a transaction set using the given parameters, combine
      // it with the given closeTime and optionally a given base fee
      // increment, and make it into a StellarValue to add to the list
      // of candidates so far.  Keep track of the hashes and sizes and
      // operation sizes of all the transaction sets, all of the close
      // times, and all of the base fee upgrades that we've seen, so that
      // we can compute the expected result of combining all the
      // candidates so far.  (We're using base fees simply as one example
      // of a type of upgrade, whose expected result is the maximum of all
      // candidates'.)
      auto [txSet, applicableTxSet] =
          makeTransactions(spec.n, spec.nbOps, spec.feeMulti);
      txSetHashes.push_back(txSet->getContentsHash());
      txSetSizes.push_back(applicableTxSet->size(lcl.header));
      txSetOpSizes.push_back(applicableTxSet->sizeOpTotal());
      closeTimes.push_back(spec.closeTime);
      if (spec.baseFeeIncrement) {
        auto const baseFee = lcl.header.baseFee + *spec.baseFeeIncrement;
        baseFees.push_back(baseFee);
        LedgerUpgrade ledgerUpgrade;
        ledgerUpgrade.type(LEDGER_UPGRADE_BASE_FEE);
        ledgerUpgrade.newBaseFee() = baseFee;
        Value upgrade(xdr::xdr_to_opaque(ledgerUpgrade));
        SVUpgrades upgrades;
        upgrades.emplace_back(upgrade.begin(), upgrade.end());
        addToCandidates(
            makeTxUpgradePair(herder, txSet, spec.closeTime, upgrades));
      } else {
        addToCandidates(makeTxPair(herder, txSet, spec.closeTime));
      }

      // Compute the expected transaction set, close time, and upgrade
      // vector resulting from combining all the candidates so far.
      auto const bestTxSetIndex =
          std::distance(txSetSizes.begin(),
                        std::max_element(txSetSizes.begin(), txSetSizes.end()));
      REQUIRE(txSetSizes.size() == closeTimes.size());
      auto const expectedHash = txSetHashes[bestTxSetIndex];
      auto const expectedCloseTime = closeTimes[bestTxSetIndex];
      SVUpgrades expectedUpgradeVector;
      if (!baseFees.empty()) {
        LedgerUpgrade expectedLedgerUpgrade;
        expectedLedgerUpgrade.type(LEDGER_UPGRADE_BASE_FEE);
        expectedLedgerUpgrade.newBaseFee() =
            *std::max_element(baseFees.begin(), baseFees.end());
        Value const expectedUpgradeValue(
            xdr::xdr_to_opaque(expectedLedgerUpgrade));
        expectedUpgradeVector.emplace_back(expectedUpgradeValue.begin(),
                                           expectedUpgradeValue.end());
      }

      // Combine all the candidates seen so far, and extract the
      // returned StellarValue.
      ValueWrapperPtr v =
          herder.getHerderSCPDriver().combineCandidates(1, candidates);
      StellarValue sv;
      xdr::xdr_from_opaque(v->getValue(), sv);

      // Compare the returned StellarValue's contents with the
      // expected ones that we computed above.
      REQUIRE(sv.ext.v() == STELLAR_VALUE_SIGNED);
      REQUIRE(sv.txSetHash == expectedHash);
      REQUIRE(sv.closeTime == expectedCloseTime);
      REQUIRE(sv.upgrades == expectedUpgradeVector);
    };

    // Test some list of candidates, comparing the output of
    // combineCandidates() and the one we compute at each step.

    std::vector<CandidateSpec> const specs{
        {0, 1, 100, 10, std::nullopt},
        {10, 1, 100, 5, std::make_optional<uint32>(1)},
        {5, 3, 100, 20, std::make_optional<uint32>(2)},
        {7, 2, 5, 30, std::make_optional<uint32>(3)}};

    std::for_each(specs.begin(), specs.end(), addCandidateThenTest);

    auto const bestTxSetIndex =
        std::distance(txSetSizes.begin(),
                      std::max_element(txSetSizes.begin(), txSetSizes.end()));
    REQUIRE(txSetOpSizes[bestTxSetIndex] == expectedOps);

    auto txSetL = makeTransactions(maxTxSetSize, 1, 101).first;
    addToCandidates(makeTxPair(herder, txSetL, 20));
    auto txSetL2 = makeTransactions(maxTxSetSize, 1, 1000).first;
    addToCandidates(makeTxPair(herder, txSetL2, 20));
    auto v = herder.getHerderSCPDriver().combineCandidates(1, candidates);
    StellarValue sv;
    xdr::xdr_from_opaque(v->getValue(), sv);
    REQUIRE(sv.ext.v() == STELLAR_VALUE_SIGNED);
    REQUIRE(sv.txSetHash == txSetL2->getContentsHash());
  }

  SECTION("validateValue signatures") {
    auto &herder = static_cast<HerderImpl &>(app->getHerder());
    auto &scp = herder.getHerderSCPDriver();
    auto seq = herder.trackingConsensusLedgerIndex() + 1;
    auto ct = app->timeNow() + 1;

    auto txSet0 = makeTransactions(0, 1, 100).first;
    {
      // make sure that txSet0 is loaded
      auto p = makeTxPair(herder, txSet0, ct);
      auto envelope = makeEnvelope(herder, p, {}, seq, true);
      REQUIRE(herder.recvSCPEnvelope(envelope) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(herder.recvTxSet(txSet0->getContentsHash(), txSet0));
    }

    SECTION("valid") {
      auto nomV = makeTxPair(herder, txSet0, ct);
      REQUIRE(scp.validateValue(seq, nomV.first, true) ==
              SCPDriver::kFullyValidatedValue);

      auto balV = makeTxPair(herder, txSet0, ct);
      REQUIRE(scp.validateValue(seq, balV.first, false) ==
              SCPDriver::kFullyValidatedValue);
    }
    SECTION("invalid") {
      auto checkInvalid = [&](StellarValue const &sv, bool nomination) {
        auto v = xdr::xdr_to_opaque(sv);
        REQUIRE(scp.validateValue(seq, v, nomination) ==
                SCPDriver::kInvalidValue);
      };

      auto testInvalidValue = [&](bool isNomination) {
        SECTION("basic value") {
          auto basicVal = StellarValue(txSet0->getContentsHash(), ct,
                                       emptyUpgradeSteps, STELLAR_VALUE_BASIC);
          checkInvalid(basicVal, isNomination);
        }
        SECTION("signed value") {
          auto p = makeTxPair(herder, txSet0, ct);
          StellarValue sv;
          xdr::xdr_from_opaque(p.first, sv);

          // mutate in a few ways
          SECTION("missing signature") {
            sv.ext.lcValueSignature().signature.clear();
            checkInvalid(sv, isNomination);
          }
          SECTION("wrong signature") {
            sv.ext.lcValueSignature().signature[0] ^= 1;
            checkInvalid(sv, isNomination);
          }
          SECTION("wrong signature 2") {
            sv.ext.lcValueSignature().nodeID.ed25519()[0] ^= 1;
            checkInvalid(sv, isNomination);
          }
        }
      };

      SECTION("nomination") { testInvalidValue(/* isNomination */ true); }
      SECTION("ballot") { testInvalidValue(/* isNomination */ false); }
    }

    SECTION("empty-tx-set hash/type mismatch") {
      auto checkInvalidMismatch = [&](StellarValue const &sv) {
        auto v = xdr::xdr_to_opaque(sv);

        REQUIRE(scp.validateValue(seq, v, true) == SCPDriver::kInvalidValue);
        REQUIRE(scp.validateValue(seq, v, false) == SCPDriver::kInvalidValue);

        ValueWrapperPtr extracted;
        REQUIRE_NOTHROW(extracted = scp.extractValidValue(seq, v));
        REQUIRE(extracted == nullptr);
      };

      SECTION("signed value with empty-tx-set hash") {
        // Create signed stellar value with empty tx set hash and
        // validate.
        StellarValue sv =
            herder.makeStellarValue(Herder::EMPTY_TX_SET_HASH, ct,
                                    emptyUpgradeSteps, root->getSecretKey());
        checkInvalidMismatch(sv);
      }

      SECTION("empty-tx-set value without empty-tx-set hash") {
        auto p = makeTxPair(herder, txSet0, ct);
        auto emptyTxSetValue = scp.makeEmptyTxSetValueFromValue(p.first);
        StellarValue sv;
        xdr::xdr_from_opaque(emptyTxSetValue, sv);
        sv.txSetHash = txSet0->getContentsHash();
        checkInvalidMismatch(sv);
      }
    }

    SECTION("valid empty-tx-set value") {
      auto p = makeTxPair(herder, txSet0, ct);
      auto emptyTxSetValue = scp.makeEmptyTxSetValueFromValue(p.first);

      bool const allowed = protocolVersionStartsFrom(
          protocolVersion, EMPTY_TX_SET_PROTOCOL_VERSION);

      // Ballot path: a well-formed empty-tx-set value is accepted only
      // once the protocol allows them. This is the assertion that
      // catches an inverted check in deserializeAndValidateStellarValue.
      REQUIRE(scp.validateValue(seq, emptyTxSetValue,
                                /*nomination=*/false) ==
              (allowed ? SCPDriver::kFullyValidatedValue
                       : SCPDriver::kInvalidValue));

      // Nomination path: empty-tx-set values are rejected by design.
      REQUIRE(scp.validateValue(seq, emptyTxSetValue,
                                /*nomination=*/true) ==
              SCPDriver::kInvalidValue);
    }
  }

  SECTION("validateValue closeTimes") {
    auto &herder = static_cast<HerderImpl &>(app->getHerder());
    auto &scp = herder.getHerderSCPDriver();

    auto const lclCloseTime = lcl.header.scpValue.closeTime;

    auto testTxBounds = [&](TimePoint const minTime, TimePoint const maxTime,
                            TimePoint const nextCloseTime,
                            bool const expectValid) {
      REQUIRE(nextCloseTime > lcl.header.scpValue.closeTime);
      // Build a transaction set containing one transaction (which
      // could be any transaction that is valid in all ways aside from
      // its time bounds) with the given minTime and maxTime.
      auto tx = makeMultiPayment(*root, *root, 10, 1000, 0, 100);
      setMinTime(tx, minTime);
      setMaxTime(tx, maxTime);
      auto &sig = tx->getMutableEnvelope().type() == ENVELOPE_TYPE_TX_V0
                      ? tx->getMutableEnvelope().v0().signatures
                      : tx->getMutableEnvelope().v1().signatures;
      sig.clear();
      tx->addSignature(root->getSecretKey());
      auto [txSet, applicableTxSet] =
          testtxset::makeNonValidatedTxSetBasedOnLedgerVersion(
              {tx}, *app,
              app->getLedgerManager().getLastClosedLedgerHeader().hash);

      // Build a StellarValue containing the transaction set we just
      // built and the given next closeTime.
      auto val = makeTxPair(herder, txSet, nextCloseTime);
      auto const seq = herder.trackingConsensusLedgerIndex() + 1;
      auto envelope = makeEnvelope(herder, val, {}, seq, true);
      REQUIRE(herder.recvSCPEnvelope(envelope) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(herder.recvTxSet(txSet->getContentsHash(), txSet));

      // Validate the StellarValue.
      SCPDriver::ValidationLevel expectedValidationLevel =
          SCPDriver::kFullyValidatedValue;
      if (!expectValid) {
        if (scp.protocolAllowsEmptyTxSetValues()) {
          // If CAP-0083 is active, then this StellarValue is
          // considered structurally valid because only the tx set is
          // invalid.
          expectedValidationLevel = SCPDriver::kStructurallyValidValue;
        } else {
          expectedValidationLevel = SCPDriver::kInvalidValue;
        }
      }
      REQUIRE(scp.validateValue(seq, val.first, true) ==
              expectedValidationLevel);

      // Confirm that getTxTrimList() as used by
      // makeTxSetFromTransactions() trims the transaction if
      // and only if we expect it to be invalid.
      auto closeTimeOffset = nextCloseTime - lclCloseTime;
      TxFrameList removed;
      UnorderedMap<AccountID, int64_t> accountFeeMap;
      TxSetUtils::trimInvalid(
          applicableTxSet->getPhase(TxSetPhase::CLASSIC).getSequentialTxs(),
          *app, accountFeeMap, closeTimeOffset, closeTimeOffset, removed);
      REQUIRE(removed.size() == (expectValid ? 0 : 1));
    };

    auto t1 = lclCloseTime + 1, t2 = lclCloseTime + 2;

    SECTION("valid in all protocols") { testTxBounds(0, t1, t1, true); }

    SECTION("invalid time bounds: expired (invalid maxTime)") {
      testTxBounds(0, t1, t2, false);
    }

    SECTION("valid time bounds: premature minTime") {
      testTxBounds(t1, 0, t1, true);
    }
  }

  SECTION("validateValue txSet cached") {
    auto &herder = static_cast<HerderImpl &>(app->getHerder());
    auto seq = herder.trackingConsensusLedgerIndex() + 1;

    auto &cache = herder.getHerderSCPDriver().getTxSetValidityCache();
    REQUIRE(cache.getCounters().mHits == 0);
    REQUIRE(cache.getCounters().mMisses == 0);

    // Triggering next ledger will construct and cache the block
    herder.triggerNextLedger(seq, true);
    // All hits during the whole SCP round. One of them is the validity
    // check that determines whether or not to replace the transaction set
    // with an empty one (CAP-0083).
    uint64_t const expectedHits = 11;
    REQUIRE(cache.getCounters().mHits == expectedHits);
    // One miss from the initial makeTxSetFromTransactions
    REQUIRE(cache.getCounters().mMisses == 1);
  }
  SECTION("accept qset and txset") {
    auto makePublicKey = [](int i) {
      auto hash = sha256("NODE_SEED_" + std::to_string(i));
      auto secretKey = SecretKey::fromSeed(hash);
      return secretKey.getPublicKey();
    };

    auto makeSingleton = [](PublicKey const &key) {
      auto result = SCPQuorumSet{};
      result.threshold = 1;
      result.validators.push_back(key);
      return result;
    };

    auto keys = std::vector<PublicKey>{};
    for (auto i = 0; i < 1001; i++) {
      keys.push_back(makePublicKey(i));
    }

    auto saneQSet1 = makeSingleton(keys[0]);
    auto saneQSet1Hash = sha256(xdr::xdr_to_opaque(saneQSet1));
    auto saneQSet2 = makeSingleton(keys[1]);
    auto saneQSet2Hash = sha256(xdr::xdr_to_opaque(saneQSet2));

    auto bigQSet = SCPQuorumSet{};
    bigQSet.threshold = 1;
    bigQSet.validators.push_back(keys[0]);
    for (auto i = 0; i < 10; i++) {
      bigQSet.innerSets.push_back({});
      bigQSet.innerSets.back().threshold = 1;
      for (auto j = i * 100 + 1; j <= (i + 1) * 100; j++)
        bigQSet.innerSets.back().validators.push_back(keys[j]);
    }
    auto bigQSetHash = sha256(xdr::xdr_to_opaque(bigQSet));

    auto &herder = static_cast<HerderImpl &>(app->getHerder());
    auto transactions1 = makeTransactions(5, 1, 100).first;
    auto transactions2 = makeTransactions(4, 1, 100).first;

    auto p1 = makeTxPair(herder, transactions1, 10);
    auto p2 = makeTxPair(herder, transactions1, 10);
    // use current + 1 to allow for any value (old values get filtered more)
    auto lseq = herder.trackingConsensusLedgerIndex() + 1;
    auto saneEnvelopeQ1T1 = makeEnvelope(herder, p1, saneQSet1Hash, lseq, true);
    auto saneEnvelopeQ1T2 = makeEnvelope(herder, p2, saneQSet1Hash, lseq, true);
    auto saneEnvelopeQ2T1 = makeEnvelope(herder, p1, saneQSet2Hash, lseq, true);
    auto bigEnvelope = makeEnvelope(herder, p1, bigQSetHash, lseq, true);

    TxSetXDRFrameConstPtr malformedTxSet;
    if (transactions1->isGeneralizedTxSet()) {
      GeneralizedTransactionSet xdrTxSet;
      transactions1->toXDR(xdrTxSet);
      auto &txs = xdrTxSet.v1TxSet()
                      .phases[0]
                      .v0Components()[0]
                      .txsMaybeDiscountedFee()
                      .txs;
      std::swap(txs[0], txs[1]);
      malformedTxSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
    } else {
      TransactionSet xdrTxSet;
      transactions1->toXDR(xdrTxSet);
      auto &txs = xdrTxSet.txs;
      std::swap(txs[0], txs[1]);
      malformedTxSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
    }
    auto malformedTxSetPair = makeTxPair(herder, malformedTxSet, 10);
    auto malformedTxSetEnvelope =
        makeEnvelope(herder, malformedTxSetPair, saneQSet1Hash, lseq, true);

    SECTION("return FETCHING until fetched") {
      REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
      REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), p1.second));
      // will not return ENVELOPE_STATUS_READY as the recvSCPEnvelope() is
      // called internally
      // when QSet and TxSet are both received
      REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
              Herder::ENVELOPE_STATUS_PROCESSED);
    }

    SECTION("only accepts qset once") {
      REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
      REQUIRE(!herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));

      SECTION("when re-receiving the same envelope") {
        REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(!herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
      }

      SECTION("when receiving different envelope with the same qset") {
        REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T2) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(!herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
      }
    }

    SECTION("only accepts txset once") {
      REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), p1.second));

      SECTION("when re-receiving the same envelope") {
        REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(!herder.recvTxSet(p1.second->getContentsHash(), p1.second));
      }

      SECTION("when receiving different envelope with the same txset") {
        REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ2T1) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(!herder.recvTxSet(p1.second->getContentsHash(), p1.second));
      }

      SECTION("when receiving envelope with malformed tx set") {
        REQUIRE(herder.recvSCPEnvelope(malformedTxSetEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(herder.recvTxSet(malformedTxSetPair.second->getContentsHash(),
                                 malformedTxSetPair.second));

        REQUIRE(herder.recvSCPEnvelope(malformedTxSetEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(!herder.recvTxSet(malformedTxSetPair.second->getContentsHash(),
                                  malformedTxSetPair.second));
      }
    }

    SECTION("do not accept unasked qset") {
      REQUIRE(!herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
      REQUIRE(!herder.recvSCPQuorumSet(saneQSet2Hash, saneQSet2));
      REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
    }

    SECTION("do not accept unasked txset") {
      REQUIRE(!herder.recvTxSet(p1.second->getContentsHash(), p1.second));
      REQUIRE(!herder.recvTxSet(p2.second->getContentsHash(), p2.second));
    }

    SECTION("do not accept not sane qset") {
      REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
    }

    SECTION("do not accept txset from envelope discarded because of unsane "
            "qset") {
      REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
      REQUIRE(!herder.recvTxSet(p1.second->getContentsHash(), p1.second));
    }

    SECTION(
        "accept txset from envelope with unsane qset before receiving qset") {
      REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), p1.second));
      REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
    }

    SECTION("accept txset from envelopes with both valid and unsane qset") {
      REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
      REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
      REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), p1.second));
    }

    SECTION("accept malformed txset, but fail validation") {
      REQUIRE(herder.recvSCPEnvelope(malformedTxSetEnvelope) ==
              Herder::ENVELOPE_STATUS_FETCHING);
      REQUIRE(herder.recvTxSet(malformedTxSetPair.second->getContentsHash(),
                               malformedTxSetPair.second));
      HerderSCPDriver &scp = herder.getHerderSCPDriver();
      REQUIRE(scp.validateValue(herder.trackingConsensusLedgerIndex() + 1,
                                malformedTxSetPair.first, false) ==
              (scp.protocolAllowsEmptyTxSetValues()
                   ? SCPDriver::kStructurallyValidValue
                   : SCPDriver::kInvalidValue));
    }
  }
}

TEST_CASE("SCP Driver", "[herder][acceptance]") {
  SECTION("previous protocol") {
    testSCPDriver(Config::CURRENT_LEDGER_PROTOCOL_VERSION - 1, 1000, 15);
  }
  SECTION("protocol current") {
    testSCPDriver(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 1000, 15);
  }
}

// Test combineCandidates handling of candidates where
// previousLedgerHash != LCL.hash
TEST_CASE("combineCandidates with mismatched previousLedgerHash candidate",
          "[herder][bug]") {
  Config cfg(getTestConfig());

  VirtualClock clock;
  auto app = createTestApplication(clock, cfg);
  auto &herder = dynamic_cast<HerderImpl &>(app->getHerder());
  auto &pe = herder.getPendingEnvelopes();
  auto &driver = herder.getHerderSCPDriver();

  auto const &lcl = app->getLedgerManager().getLastClosedLedgerHeader();
  uint32_t const ver = lcl.header.ledgerVersion;
  uint64_t const closeTime = lcl.header.scpValue.closeTime + 1;
  uint64_t const slotIndex = lcl.header.ledgerSeq + 1;

  // Two structurally-valid empty tx sets that differ only in
  // previousLedgerHash.
  auto goodTxSet = TxSetXDRFrame::makeEmpty(lcl.hash, ver); // matches LCL
  auto badTxSet = TxSetXDRFrame::makeEmpty(sha256("not the LCL hash"),
                                           ver); // mismatched

  ValueWrapperPtrSet candidates;
  // Register the tx set so combineCandidates' getTxSet() returns it, then add
  // a candidate value referencing it.
  auto addCandidate = [&](TxSetXDRFrameConstPtr const &txSet) {
    pe.addTxSet(txSet->getContentsHash(), slotIndex, txSet);
    StellarValue sv = herder.makeStellarValue(
        txSet->getContentsHash(), closeTime, emptyUpgradeSteps, cfg.NODE_SEED);
    candidates.emplace(driver.wrapValue(xdr::xdr_to_opaque(sv)));
  };
  auto combinedTxSetHash = [&]() {
    ValueWrapperPtr result = driver.combineCandidates(slotIndex, candidates);
    StellarValue sv;
    xdr::xdr_from_opaque(result->getValue(), sv);
    return sv.txSetHash;
  };

  SECTION("prefer applicable candidate over mismatched candidate") {
    addCandidate(goodTxSet);
    addCandidate(badTxSet);
    REQUIRE(combinedTxSetHash() == goodTxSet->getContentsHash());
  }

  SECTION("all candidates have mismatched previousLedgerHash") {
    // If the *only* option is a candidate with a mismatched
    // previousLedgerHash, choose it.
    addCandidate(badTxSet);
    REQUIRE(combinedTxSetHash() == badTxSet->getContentsHash());
  }
}

#ifdef BUILD_THREAD_JITTER
#endif

namespace {
// The main purpose of this test is to ensure the externalize path works
// correctly. This entails properly updating tracking in Herder, forwarding
// externalize information to LM, and Herder appropriately reacting to ledger
// close.

} // namespace

TEST_CASE("slot herder policy", "[herder]") {
  SIMULATION_CREATE_NODE(0);
  SIMULATION_CREATE_NODE(1);
  SIMULATION_CREATE_NODE(2);
  SIMULATION_CREATE_NODE(3);

  Config cfg(getTestConfig());

  // start in sync
  cfg.FORCE_SCP = false;
  cfg.MANUAL_CLOSE = false;
  cfg.NODE_SEED = v0SecretKey;
  cfg.MAX_SLOTS_TO_REMEMBER = 5;
  cfg.NODE_IS_VALIDATOR = false;

  cfg.QUORUM_SET.threshold = 3; // 3 out of 4
  cfg.QUORUM_SET.validators.push_back(v1NodeID);
  cfg.QUORUM_SET.validators.push_back(v2NodeID);
  cfg.QUORUM_SET.validators.push_back(v3NodeID);

  VirtualClock clock;
  Application::pointer app = createTestApplication(clock, cfg);

  auto &herder = static_cast<HerderImpl &>(app->getHerder());

  auto qSet = herder.getSCP().getLocalQuorumSet();
  auto qsetHash = sha256(xdr::xdr_to_opaque(qSet));

  auto recvExternalize = [&](SecretKey const &sk, uint64_t slotIndex,
                             Hash const &prevHash) {
    auto envelope = SCPEnvelope{};
    envelope.statement.slotIndex = slotIndex;
    envelope.statement.pledges.type(SCP_ST_EXTERNALIZE);
    auto &ext = envelope.statement.pledges.externalize();
    TxSetXDRFrameConstPtr txSet = TxSetXDRFrame::makeEmpty(
        app->getLedgerManager().getLastClosedLedgerHeader());

    // sign values with the same secret key
    StellarValue sv =
        herder.makeStellarValue(txSet->getContentsHash(), (TimePoint)slotIndex,
                                xdr::xvector<UpgradeType, 6>{}, v1SecretKey);
    ext.commit.counter = 1;
    ext.commit.value = xdr::xdr_to_opaque(sv);
    ext.commitQuorumSetHash = qsetHash;
    ext.nH = 1;
    envelope.statement.nodeID = sk.getPublicKey();
    herder.signEnvelope(sk, envelope);
    auto res = herder.recvSCPEnvelope(envelope, qSet, txSet);
    REQUIRE(res == Herder::ENVELOPE_STATUS_READY);
  };

  auto const LIMIT = cfg.MAX_SLOTS_TO_REMEMBER;

  auto recvExternPeers = [&](uint32 seq, Hash const &prev, bool quorum) {
    recvExternalize(v1SecretKey, seq, prev);
    recvExternalize(v2SecretKey, seq, prev);
    if (quorum) {
      recvExternalize(v3SecretKey, seq, prev);
    }
  };
  // first, close a few ledgers, see if we actually retain the right
  // number of ledgers
  auto timeout = clock.now() + std::chrono::minutes(10);
  for (uint32 i = 0; i < LIMIT * 2; ++i) {
    auto seq = app->getLedgerManager().getLastClosedLedgerNum() + 1;
    auto prev = app->getLedgerManager().getLastClosedLedgerHeader().hash;
    recvExternPeers(seq, prev, true);
    while (app->getLedgerManager().getLastClosedLedgerNum() < seq) {
      clock.crank(true);
      REQUIRE(clock.now() < timeout);
    }
  }
  REQUIRE(herder.getState() == Herder::HERDER_TRACKING_NETWORK_STATE);
  REQUIRE(herder.getSCP().getKnownSlotsCount() == LIMIT);

  auto oneSec = std::chrono::seconds(1);
  // let the node go out of sync, it should reach the desired state
  timeout = clock.now() + Herder::CONSENSUS_STUCK_TIMEOUT_SECONDS + oneSec;
  while (herder.getState() == Herder::HERDER_TRACKING_NETWORK_STATE) {
    clock.crank(false);
    REQUIRE(clock.now() < timeout);
  }

  auto const PARTIAL = Herder::LEDGER_VALIDITY_BRACKET;
  // create a gap
  auto newSeq = app->getLedgerManager().getLastClosedLedgerNum() + 2;
  for (uint32 i = 0; i < PARTIAL; ++i) {
    auto prev = app->getLedgerManager().getLastClosedLedgerHeader().hash;
    // advance clock to ensure that ct is valid
    clock.sleep_for(oneSec);
    recvExternPeers(newSeq++, prev, false);
  }
  REQUIRE(herder.getSCP().getKnownSlotsCount() == (LIMIT + PARTIAL));

  timeout = clock.now() + Herder::OUT_OF_SYNC_RECOVERY_TIMER + oneSec;
  while (herder.getSCP().getKnownSlotsCount() !=
         Herder::LEDGER_VALIDITY_BRACKET) {
    clock.sleep_for(oneSec);
    clock.crank(false);
    REQUIRE(clock.now() < timeout);
  }

  Hash prevHash;
  // add a bunch more - not v-blocking
  for (uint32 i = 0; i < LIMIT; ++i) {
    recvExternalize(v1SecretKey, newSeq++, prevHash);
  }
  // policy here is to not do anything
  auto waitForRecovery = [&]() {
    timeout = clock.now() + Herder::OUT_OF_SYNC_RECOVERY_TIMER + oneSec;
    while (clock.now() < timeout) {
      clock.sleep_for(oneSec);
      clock.crank(false);
    }
  };

  waitForRecovery();
  auto const FULLSLOTS = Herder::LEDGER_VALIDITY_BRACKET + LIMIT;
  REQUIRE(herder.getSCP().getKnownSlotsCount() == FULLSLOTS);

  // now inject a few more, policy should apply here, with
  // partial in between
  // lower slots getting dropped so the total number of slots in memory is
  // constant
  auto cutOff = Herder::LEDGER_VALIDITY_BRACKET - 1;
  for (uint32 i = 0; i < cutOff; ++i) {
    recvExternPeers(newSeq++, prevHash, false);
    waitForRecovery();
    REQUIRE(herder.getSCP().getKnownSlotsCount() == FULLSLOTS);
  }
  // adding one more, should get rid of the partial slots
  recvExternPeers(newSeq++, prevHash, false);
  waitForRecovery();
  REQUIRE(herder.getSCP().getKnownSlotsCount() ==
          Herder::LEDGER_VALIDITY_BRACKET);
}

using Topology = std::pair<std::vector<SecretKey>, std::vector<ValidatorEntry>>;

// Generate a Topology with a single org containing 3 validators of HIGH quality
static Topology simpleThreeNode() {
  // Generate validators
  std::vector<SecretKey> sks;
  std::vector<ValidatorEntry> validators;
  int constexpr numValidators = 3;
  for (int i = 0; i < numValidators; ++i) {
    SecretKey const &key =
        sks.emplace_back(SecretKey::pseudoRandomForTesting());
    ValidatorEntry &entry = validators.emplace_back();
    entry.mName = fmt::format("validator-{}", i);
    entry.mHomeDomain = "A";
    entry.mQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
    entry.mKey = key.getPublicKey();
    entry.mHasHistory = false;
  }
  return {sks, validators};
}

// Generate a topology with 3 orgs of HIGH quality. Two orgs have 3 validators
// and one org has 5 validators.
static Topology unbalancedOrgs() {
  // Generate validators
  std::vector<SecretKey> sks;
  std::vector<ValidatorEntry> validators;
  int constexpr numValidators = 11;
  for (int i = 0; i < numValidators; ++i) {
    // Orgs A and B have 3 validators each. Org C has 5 validators.
    std::string org = "C";
    if (i < 3) {
      org = "A";
    } else if (i < 6) {
      org = "B";
    }

    SecretKey const &key =
        sks.emplace_back(SecretKey::pseudoRandomForTesting());
    ValidatorEntry &entry = validators.emplace_back();
    entry.mName = fmt::format("validator-{}", i);
    entry.mHomeDomain = org;
    entry.mQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
    entry.mKey = key.getPublicKey();
    entry.mHasHistory = false;
  }
  return {sks, validators};
}

// Generate a tier 1-like topology. This topology has 7 HIGH quality orgs, each
// with 3 validators.
static Topology tier1Like() {
  std::vector<SecretKey> sks;
  std::vector<ValidatorEntry> validators;
  int constexpr numOrgs = 7;
  int constexpr validatorsPerOrg = 3;

  for (int i = 0; i < numOrgs; ++i) {
    std::string const org = fmt::format("org-{}", i);
    for (int j = 0; j < validatorsPerOrg; ++j) {
      SecretKey const &key =
          sks.emplace_back(SecretKey::pseudoRandomForTesting());
      ValidatorEntry &entry = validators.emplace_back();
      entry.mName = fmt::format("validator-{}-{}", i, j);
      entry.mHomeDomain = org;
      entry.mQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
      entry.mKey = key.getPublicKey();
      entry.mHasHistory = false;
    }
  }

  return {sks, validators};
}

// Generate a slightly unbalanced topology. This topology has 7 HIGH quality
// orgs, 6 of which have 3 validators and 1 has 5 validators.
static Topology slightlyUnbalancedOrgs() {
  std::vector<SecretKey> sks;
  std::vector<ValidatorEntry> validators;
  int constexpr numOrgs = 7;

  for (int i = 0; i < numOrgs; ++i) {
    std::string const org = fmt::format("org-{}", i);
    int const numValidators = i == 0 ? 5 : 3;
    for (int j = 0; j < numValidators; ++j) {
      SecretKey const &key =
          sks.emplace_back(SecretKey::pseudoRandomForTesting());
      ValidatorEntry &entry = validators.emplace_back();
      entry.mName = fmt::format("validator-{}-{}", i, j);
      entry.mHomeDomain = org;
      entry.mQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
      entry.mKey = key.getPublicKey();
      entry.mHasHistory = false;
    }
  }

  return {sks, validators};
}

// Returns a random quality up to `maxQuality`
static ValidatorQuality randomQuality(ValidatorQuality maxQuality) {
  return static_cast<ValidatorQuality>(rand_uniform<int>(
      static_cast<int>(ValidatorQuality::VALIDATOR_LOW_QUALITY),
      static_cast<int>(maxQuality)));
}

// Returns the minimum size an org of quality `q` can have
static int constexpr minOrgSize(ValidatorQuality q) {
  switch (q) {
  case ValidatorQuality::VALIDATOR_LOW_QUALITY:
  case ValidatorQuality::VALIDATOR_MED_QUALITY:
    return 1;
  case ValidatorQuality::VALIDATOR_HIGH_QUALITY:
  case ValidatorQuality::VALIDATOR_CRITICAL_QUALITY:
    return 3;
  }
}

// Generate a random topology with up to `maxValidators` validators. Ensures at
// least one org is HIGH quality.
static Topology randomTopology(int maxValidators) {
  int const numValidators = rand_uniform<int>(3, maxValidators);
  int constexpr minCritOrgSize =
      minOrgSize(ValidatorQuality::VALIDATOR_CRITICAL_QUALITY);

  // Generate validators
  int curOrg = 0;
  int curOrgSize = 0;
  ValidatorQuality curQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
  std::vector<SecretKey> sks(numValidators);
  std::vector<ValidatorEntry> validators(numValidators);
  for (int i = 0; i < numValidators; ++i) {
    if (curOrgSize >= minOrgSize(curQuality) && rand_flip()) {
      // Start new org
      ++curOrg;
      curOrgSize = 0;
      curQuality =
          randomQuality(numValidators - i >= minCritOrgSize
                            ? ValidatorQuality::VALIDATOR_CRITICAL_QUALITY
                            : ValidatorQuality::VALIDATOR_MED_QUALITY);
    }

    std::string const org = fmt::format("org-{}", curOrg);
    SecretKey const &key = sks.at(i) = SecretKey::pseudoRandomForTesting();

    ValidatorEntry &entry = validators.at(i);
    entry.mName = fmt::format("validator-{}", i);
    entry.mHomeDomain = org;
    entry.mQuality = curQuality;
    entry.mKey = key.getPublicKey();
    entry.mHasHistory = false;

    ++curOrgSize;
  }

  return {sks, validators};
}

// Expected weight of an org with quality `orgQuality` in a topology with a max
// quality of `maxQuality` and or quality counts of `orgQualityCounts`. This
// function normalizes the weight so that the highest quality has a weight of
// `1`.
static double expectedOrgNormalizedWeight(
    std::unordered_map<ValidatorQuality, uint64> const &orgQualityCounts,
    ValidatorQuality maxQuality, ValidatorQuality orgQuality) {
  if (orgQuality == ValidatorQuality::VALIDATOR_LOW_QUALITY) {
    return 0.0;
  }

  double normalizedWeight = 1.0;

  // For each quality level higher than `orgQuality`, divide the weight by 10
  // times the number of orgs at that quality level
  for (int q = static_cast<int>(maxQuality); q > static_cast<int>(orgQuality);
       --q) {
    normalizedWeight /=
        10 * orgQualityCounts.at(static_cast<ValidatorQuality>(q));
  }
  return normalizedWeight;
}

// Expected weight of a validator in an org of size `orgSize` with quality
// `orgQuality`.  `maxQuality` is the maximum quality present in the
// configuration. This function normalizes the weight so that the highest
// organization-level quality has a weight of `1`.
static double expectedNormalizedWeight(
    std::unordered_map<ValidatorQuality, uint64> const &orgQualityCounts,
    ValidatorQuality maxQuality, ValidatorQuality orgQuality, int orgSize) {
  return expectedOrgNormalizedWeight(orgQualityCounts, maxQuality, orgQuality) /
         orgSize;
}

// Collect information about the qualities and sizes of organizations in
// `validators` and store them in `maxQuality`, `orgQualities`, `orgSizes`, and
// `orgQualityCounts`.
static void
collectOrgInfo(ValidatorQuality &maxQuality,
               std::unordered_map<std::string, ValidatorQuality> &orgQualities,
               std::unordered_map<std::string, int> &orgSizes,
               std::unordered_map<ValidatorQuality, uint64> &orgQualityCounts,
               std::vector<ValidatorEntry> const &validators) {
  maxQuality = ValidatorQuality::VALIDATOR_LOW_QUALITY;
  ValidatorQuality minQuality = ValidatorQuality::VALIDATOR_CRITICAL_QUALITY;
  std::unordered_map<ValidatorQuality, std::unordered_set<std::string>>
      orgsByQuality;
  for (ValidatorEntry const &validator : validators) {
    maxQuality = std::max(maxQuality, validator.mQuality);
    minQuality = std::min(minQuality, validator.mQuality);
    orgQualities[validator.mHomeDomain] = validator.mQuality;
    ++orgSizes[validator.mHomeDomain];
    orgsByQuality[validator.mQuality].insert(validator.mHomeDomain);
  }

  // Count orgs at each quality level
  for (int q = static_cast<int>(minQuality); q <= static_cast<int>(maxQuality);
       ++q) {
    orgQualityCounts[static_cast<ValidatorQuality>(q)] =
        orgsByQuality[static_cast<ValidatorQuality>(q)].size();
    if (q != static_cast<int>(minQuality)) {
      // Add virtual org covering next lower quality level
      ++orgQualityCounts[static_cast<ValidatorQuality>(q)];
    }
  }
}

// Given a list of validators, test that the weights of the validators herder
// reports are correct
static void testWeights(std::vector<ValidatorEntry> const &validators) {
  Config cfg = getTestConfig(0);

  cfg.generateQuorumSetForTesting(validators);

  VirtualClock clock;
  Application::pointer app = createTestApplication(clock, cfg);

  // Collect info about orgs
  ValidatorQuality maxQuality;
  std::unordered_map<std::string, ValidatorQuality> orgQualities;
  std::unordered_map<std::string, int> orgSizes;
  std::unordered_map<ValidatorQuality, uint64> orgQualityCounts;
  collectOrgInfo(maxQuality, orgQualities, orgSizes, orgQualityCounts,
                 validators);

  // Check per-validator weights
  HerderImpl &herder = dynamic_cast<HerderImpl &>(app->getHerder());
  std::unordered_map<std::string, double> normalizedOrgWeights;
  for (ValidatorEntry const &validator : validators) {
    uint64_t weight = herder.getHerderSCPDriver().getNodeWeight(
        validator.mKey, cfg.QUORUM_SET, false);
    double normalizedWeight =
        static_cast<double>(weight) / static_cast<double>(UINT64_MAX);
    normalizedOrgWeights[validator.mHomeDomain] += normalizedWeight;

    std::string const &org = validator.mHomeDomain;
    REQUIRE_THAT(
        normalizedWeight,
        Catch::Matchers::WithinAbs(
            expectedNormalizedWeight(orgQualityCounts, maxQuality,
                                     orgQualities.at(org), orgSizes.at(org)),
            0.0001));
  }

  // Check per-org weights
  for (auto const &[org, weight] : normalizedOrgWeights) {
    REQUIRE_THAT(weight,
                 Catch::Matchers::WithinAbs(
                     expectedOrgNormalizedWeight(orgQualityCounts, maxQuality,
                                                 orgQualities.at(org)),
                     0.0001));
  }
}

// Test that HerderSCPDriver::getNodeWeight produces weights that result in a
// fair distribution of nomination wins.
TEST_CASE("getNodeWeight", "[herder]") {
  SECTION("3 tier 1 validators, 1 org") {
    testWeights(simpleThreeNode().second);
  }

  SECTION("11 tier 1 validators, 3 unbalanced orgs") {
    testWeights(unbalancedOrgs().second);
  }

  SECTION("Tier1-like topology") { testWeights(tier1Like().second); }

  SECTION("Tier1-like topology with a single unbalanced org") {
    testWeights(slightlyUnbalancedOrgs().second);
  }

  SECTION("Random topology") {
    // Test weights for 1000 random topologies of up to 200 validators
    for (int i = 0; i < 1000; ++i) {
      testWeights(randomTopology(200).second);
    }
  }
}

static Value getRandomValue() {
  auto h = sha256(fmt::format("value {}", getGlobalRandomEngine()()));
  return xdr::xdr_to_opaque(h);
}

// A test version of NominationProtocol that exposes `updateRoundLeaders`
class TestNominationProtocol : public NominationProtocol {
public:
  TestNominationProtocol(Slot &slot) : NominationProtocol(slot) {}

  std::set<NodeID> const &updateRoundLeadersForTesting(
      std::optional<Value> const &previousValue = std::nullopt) {
    mPreviousValue = previousValue.value_or(getRandomValue());
    updateRoundLeaders();
    return getLeaders();
  }

  // Detect fast timeouts by examining the final round number
  bool fastTimedOut() const { return mRoundNumber > 0; }
};

// Test nomination over `numLedgers` slots. After running, check that the win
// percentages of each node and org are within 5% of the expected win
// percentages.
static void testWinProbabilities(std::vector<SecretKey> const &sks,
                                 std::vector<ValidatorEntry> const &validators,
                                 int const numLedgers) {
  REQUIRE(sks.size() == validators.size());

  // Collect info about orgs
  ValidatorQuality maxQuality;
  std::unordered_map<std::string, ValidatorQuality> orgQualities;
  std::unordered_map<std::string, int> orgSizes;
  std::unordered_map<ValidatorQuality, uint64> orgQualityCounts;
  collectOrgInfo(maxQuality, orgQualities, orgSizes, orgQualityCounts,
                 validators);

  // Generate a config
  Config cfg = getTestConfig();
  cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
  cfg.generateQuorumSetForTesting(validators);
  cfg.NODE_SEED = sks.front();

  // Create an application
  VirtualClock clock;
  Application::pointer app = createTestApplication(clock, cfg);

  // Run for `numLedgers` slots, recording the number of times each
  // node wins nomination
  UnorderedMap<NodeID, int> publishCounts;
  HerderImpl &herder = dynamic_cast<HerderImpl &>(app->getHerder());
  SCP &scp = herder.getSCP();
  int fastTimeouts = 0;
  for (int i = 0; i < numLedgers; ++i) {
    auto s = std::make_shared<Slot>(i, scp);
    TestNominationProtocol np(*s);

    std::set<NodeID> const &leaders = np.updateRoundLeadersForTesting();
    REQUIRE(leaders.size() == 1);
    for (NodeID const &leader : leaders) {
      ++publishCounts[leader];
    }

    if (np.fastTimedOut()) {
      ++fastTimeouts;
    }
  }

  CLOG_INFO(Herder, "Fast Timeouts: {} ({}%)", fastTimeouts,
            fastTimeouts * 100.0 / numLedgers);

  // Compute total expected normalized weight across all nodes
  double totalNormalizedWeight = 0.0;
  for (ValidatorEntry const &validator : validators) {
    totalNormalizedWeight += expectedNormalizedWeight(
        orgQualityCounts, maxQuality, orgQualities.at(validator.mHomeDomain),
        orgSizes.at(validator.mHomeDomain));
  }

  // Check validator win rates
  std::map<std::string, int> orgPublishCounts;
  for (ValidatorEntry const &validator : validators) {
    NodeID const &nodeID = validator.mKey;
    int publishCount = publishCounts[nodeID];

    // Compute and report node's win rate
    double winRate = static_cast<double>(publishCount) / numLedgers;
    CLOG_INFO(Herder, "Node {} win rate: {} (published {} ledgers)",
              cfg.toShortString(nodeID), winRate, publishCount);

    // Expected win rate is `weight / total weight`
    double expectedWinRate =
        expectedNormalizedWeight(orgQualityCounts, maxQuality,
                                 orgQualities.at(validator.mHomeDomain),
                                 orgSizes.at(validator.mHomeDomain)) /
        totalNormalizedWeight;

    // Check that actual win rate is within .05 of expected win
    // rate.
    REQUIRE_THAT(winRate, Catch::Matchers::WithinAbs(expectedWinRate, 0.05));

    // Record org publish counts for the next set of checks
    orgPublishCounts[validator.mHomeDomain] += publishCount;
  }

  // Check org win rates
  for (auto const &[org, count] : orgPublishCounts) {
    // Compute and report org's win rate
    double winRate = static_cast<double>(count) / numLedgers;
    CLOG_INFO(Herder, "Org {} win rate: {} (published {} ledgers)", org,
              winRate, count);

    // Expected win rate is `weight / total weight`
    double expectedWinRate =
        expectedOrgNormalizedWeight(orgQualityCounts, maxQuality,
                                    orgQualities.at(org)) /
        totalNormalizedWeight;

    // Check that actual win rate is within .05 of expected win
    // rate.
    REQUIRE_THAT(winRate, Catch::Matchers::WithinAbs(expectedWinRate, 0.05));
  }
}

// Test that the nomination algorithm produces a fair distribution of ledger
// publishers.
TEST_CASE("Fair nomination win rates", "[herder]") {
  SECTION("3 tier 1 validators, 1 org") {
    auto [sks, validators] = simpleThreeNode();
    testWinProbabilities(sks, validators, 10000);
  }

  SECTION("11 tier 1 validators, 3 unbalanced orgs") {
    auto [sks, validators] = unbalancedOrgs();
    testWinProbabilities(sks, validators, 10000);
  }

  SECTION("Tier 1-like topology") {
    auto [sks, validators] = tier1Like();
    testWinProbabilities(sks, validators, 10000);
  }

  SECTION("Tier 1-like topology with a single unbalanced org") {
    auto [sks, validators] = slightlyUnbalancedOrgs();
    testWinProbabilities(sks, validators, 10000);
  }

  SECTION("Random topology") {
    for (int i = 0; i < 10; ++i) {
      auto [sks, validators] = randomTopology(50);
      testWinProbabilities(sks, validators, 10000);
    }
  }
}

namespace {
// Returns a new `Topology` with the last org in `t` replaced with a new org
// with 3 validators. Requires that the last org in `t` have 3 validators and be
// contiguous at the back of the validators vecto.
Topology replaceOneOrg(Topology const &t) {
  Topology t2(t); // Copy the topology
  auto &[sks, validators] = t2;
  REQUIRE(sks.size() == validators.size());

  // Give the org a unique name
  std::string const orgName = "org-replaced";

  // Double check that the new org name is unique
  for (ValidatorEntry const &v : validators) {
    REQUIRE(v.mHomeDomain != orgName);
  }

  // Remove the last org
  constexpr int validatorsPerOrg = 3;
  sks.resize(sks.size() - validatorsPerOrg);
  validators.resize(validators.size() - validatorsPerOrg);

  // Add new org with 3 validators
  int constexpr numValidators = 3;
  for (int j = 0; j < numValidators; ++j) {
    SecretKey const &key =
        sks.emplace_back(SecretKey::pseudoRandomForTesting());
    ValidatorEntry &entry = validators.emplace_back();
    entry.mName = fmt::format("validator-replaced-{}", j);
    entry.mHomeDomain = orgName;
    entry.mQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
    entry.mKey = key.getPublicKey();
    entry.mHasHistory = false;
  }

  return {sks, validators};
}

// Add `orgsToAdd` new orgs to the topology `t`. Each org will have 3
// validators.
Topology addOrgs(int orgsToAdd, Topology const &t) {
  Topology t2(t); // Copy the topology
  auto &[sks, validators] = t2;
  REQUIRE(sks.size() == validators.size());

  // Generate new orgs
  for (int i = 0; i < orgsToAdd; ++i) {
    std::string const org = fmt::format("new-org-{}", i);
    int constexpr numValidators = 3;
    for (int j = 0; j < numValidators; ++j) {
      SecretKey const &key =
          sks.emplace_back(SecretKey::pseudoRandomForTesting());
      ValidatorEntry &entry = validators.emplace_back();
      entry.mName = fmt::format("new-validator-{}-{}", i, j);
      entry.mHomeDomain = org;
      entry.mQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
      entry.mKey = key.getPublicKey();
      entry.mHasHistory = false;
    }
  }
  return t2;
}

// Returns `true` if the set intersection of `leaders1` and `leaders2` is not
// empty.
bool leadersIntersect(std::set<NodeID> const &leaders1,
                      std::set<NodeID> const &leaders2) {
  std::vector<NodeID> intersection;
  std::set_intersection(leaders1.begin(), leaders1.end(), leaders2.begin(),
                        leaders2.end(), std::back_inserter(intersection));
  return !intersection.empty();
}

// Given two quorum sets consisting of validators in `validators1` and
// `validators2`, this function returns the probability that the two quorum sets
// will agree on a leader in the first round of nomination.
double computeExpectedFirstRoundAgreementProbability(
    std::vector<ValidatorEntry> const &validators1,
    std::vector<ValidatorEntry> const &validators2) {
  // Gather orgs
  std::set<std::string> orgs1;
  std::transform(validators1.begin(), validators1.end(),
                 std::inserter(orgs1, orgs1.end()),
                 [](ValidatorEntry const &v) { return v.mHomeDomain; });
  std::set<std::string> orgs2;
  std::transform(validators2.begin(), validators2.end(),
                 std::inserter(orgs2, orgs2.end()),
                 [](ValidatorEntry const &v) { return v.mHomeDomain; });

  // Compute overlap
  std::vector<std::string> sharedOrgs;
  std::set_intersection(orgs1.begin(), orgs1.end(), orgs2.begin(), orgs2.end(),
                        std::back_inserter(sharedOrgs));

  // Probability of agreement in first round is (orgs overlapping / orgs1) *
  // (orgs overlapping / orgs2). That's the probability that the two sides
  // will pick any overlapping org. The algorithm guarantees that if they pick
  // overlapping validator, they'll pick the same validator.
  double overlap = static_cast<double>(sharedOrgs.size());
  return overlap / orgs1.size() * overlap / orgs2.size();
}

// Test that the nomination algorithm behaves as expected when the two quorum
// sets `qs1` and `qs2` are not equivalent. This function requires that both
// quorum sets overlap, and contain only a single quality level of validators.
// Runs simulation for `numLedgers` slots.
// NOTE: This test counts any failure to agree on a leader as a timeout. In
// practice, it's possible that one side of the split is large enough to proceed
// without the other side. In this case, the larger side might not experience a
// timeout and "drag" the other side through consensus with it. However, this
// test aims to analyze the worst case scenario where the two sides are fairly
// balanced and real-world networking conditions are in place (some nodes
// lagging, etc), such that disagreement always results in a timeout.
void testAsymmetricTimeouts(Topology const &qs1, Topology const &qs2,
                            int const numLedgers) {
  auto const &[sks1, validators1] = qs1;
  auto const &[sks2, validators2] = qs2;

  REQUIRE(sks1.size() == validators1.size());
  REQUIRE(sks2.size() == validators2.size());

  // Generate configs and nodes representing one validator with each quorum
  // set
  std::vector<VirtualClock> clocks(2);
  std::vector<Application::pointer> apps;
  for (int i = 0; i < 2; ++i) {
    Config cfg = getTestConfig(i);
    cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
    cfg.generateQuorumSetForTesting(i == 0 ? validators1 : validators2);
    cfg.NODE_SEED = i == 0 ? sks1.back() : sks2.back();

    auto app = apps.emplace_back(createTestApplication(clocks.at(i), cfg));
  }

  // Run the nomination algorithm for `numLedgers` slots. Simulate timeouts by
  // re-running slots that don't agree on a leader until their leader
  // elections overlap. Record the number of timeouts it takes for the two
  // quorum sets to agree on a leader in `timeouts`, which is effectively a
  // mapping from number of timeouts to the number of ledgers that experienced
  // that many timeouts.
  std::vector<int> timeouts(std::max(validators1.size(), validators2.size()));
  for (int i = 0; i < numLedgers; ++i) {
    Value const v = getRandomValue();
    SCP &scp1 = dynamic_cast<HerderImpl &>(apps.at(0)->getHerder()).getSCP();
    SCP &scp2 = dynamic_cast<HerderImpl &>(apps.at(1)->getHerder()).getSCP();
    auto s1 = std::make_shared<Slot>(i, scp1);
    auto s2 = std::make_shared<Slot>(i, scp2);

    TestNominationProtocol np1(*s1);
    TestNominationProtocol np2(*s2);

    for (int j = 0; j < timeouts.size(); ++j) {
      std::set<NodeID> const &leaders1 = np1.updateRoundLeadersForTesting(v);
      std::set<NodeID> const &leaders2 = np2.updateRoundLeadersForTesting(v);
      REQUIRE(leaders1.size() == j + 1);
      REQUIRE(leaders2.size() == j + 1);

      if (leadersIntersect(leaders1, leaders2)) {
        // Agreed on a leader! Record the number of timeouts resulted.
        ++timeouts.at(j);
        break;
      }
    }

    // If leaders don't intersect after running through the loop then the
    // two quorum sets have no overlap and the test is broken.
    REQUIRE(leadersIntersect(np1.getLeaders(), np2.getLeaders()));
  }

  // For the first round, we can easily compute the expected agreement
  // probability. For subsequent rounds, we check only that the success rate
  // increases over time (modulo some small epsilon).
  double expectedSuccessRate =
      computeExpectedFirstRoundAgreementProbability(validators1, validators2);

  // Allow for some small decrease in success rate from the theoretical value.
  // We're working with probabilistic simulation here so we can't be too
  // strict or the test will be flaky.
  double constexpr epsilon = 0.1;

  // There's not enough data in the tail of the distribution to allow us to
  // assert that the success rate is what's expected. To avoid sporadic test
  // failures, we cut off `tailCutoffPoint` of the tail of the distribution
  // for the purposes of asserting test values. However, the test will still
  // log those success rates for manual examination.
  double constexpr tailCutoffPoint = 0.05;

  int numLedgersRemaining = numLedgers;
  for (int i = 0; i < timeouts.size(); ++i) {
    int const numTimeouts = timeouts.at(i);
    if (numTimeouts == 0) {
      // Avoid cluttering output
      continue;
    }

    CLOG_INFO(Herder, "Ledgers with {} timeouts: {} ({}%)", i, numTimeouts,
              static_cast<double>(numTimeouts) * 100 / numLedgers);

    if (numLedgersRemaining > numLedgers * tailCutoffPoint) {
      // Check that success rate increases over time. Allow some epsilon
      // decrease because this is a probabilistic simulation. Also stop
      // checking when we're at the last `tailCutoffPoint` timeouts as the
      // data is too sparse to be useful.
      double successRate =
          static_cast<double>(timeouts.at(i)) / numLedgersRemaining;
      REQUIRE(successRate > expectedSuccessRate - epsilon);

      // Take max of success rate and previous success rate to avoid
      // accidentally accepting a declining success rate due to episilon.
      expectedSuccessRate = std::max(successRate, expectedSuccessRate);
      numLedgersRemaining -= numTimeouts;
    }
  }
}
} // namespace

// Test timeouts with asymmetric quorums. This test serves two purposes:
// 1. It contains assertions checking for moderate (10%) deviations from the
//    expected behavior of the nomination algorithm. These should detect any
//    major issues/regressions with the algorithm.
// 2. It logs the distributions of timeouts for manual inspection. This is
//    useful for understanding the behavior of the algorithm and for testing
//    specific scenarios one might be interested in (e.g., if tier 1 disagrees
//    on one org's presence in tier 1, what is the impact on nomination
//    timeouts?).
// NOTE: This provides a worst-case analysis of timeouts. See the NOTE on
// `testAsymmetricTimeouts` for more details.
TEST_CASE("Asymmetric quorum timeouts", "[herder]") {
  // Number of slots to run for
  int constexpr numLedgers = 20000;

  SECTION("Tier 1-like topology with replaced org") {
    auto t = tier1Like();
    testAsymmetricTimeouts(t, replaceOneOrg(t), numLedgers);
  }

  SECTION("Tier 1-like topology with 1 added org") {
    auto t = tier1Like();
    testAsymmetricTimeouts(t, addOrgs(1, t), numLedgers);
  }

  SECTION("Tier 1-like topology with 3 added orgs") {
    auto t = tier1Like();
    testAsymmetricTimeouts(t, addOrgs(3, t), numLedgers);
  }
}

// Test that the nomination algorithm behaves as expected when a random
// `numUnresponsive` set of nodes in `qs` are unresponsive.  Runs simulation for
// `numLedgers` slots.
static void testUnresponsiveTimeouts(Topology const &qs, int numUnresponsive,
                                     int const numLedgers) {
  auto const &[sks, validators] = qs;
  REQUIRE(sks.size() == validators.size());
  REQUIRE(numUnresponsive < validators.size());

  // extract and shuffle node ids. Choose `numUnresponsive` nodes to be the
  // unresponsive nodes.
  std::vector<NodeID> nodeIDs;
  std::transform(validators.begin(), validators.end(),
                 std::back_inserter(nodeIDs),
                 [](ValidatorEntry const &v) { return v.mKey; });
  stellar::shuffle(nodeIDs.begin(), nodeIDs.end(), getGlobalRandomEngine());
  std::set<NodeID> unresponsive(nodeIDs.begin(),
                                nodeIDs.begin() + numUnresponsive);

  // Collect info about orgs
  ValidatorQuality maxQuality;
  std::unordered_map<std::string, ValidatorQuality> orgQualities;
  std::unordered_map<std::string, int> orgSizes;
  std::unordered_map<ValidatorQuality, uint64> orgQualityCounts;
  collectOrgInfo(maxQuality, orgQualities, orgSizes, orgQualityCounts,
                 validators);

  // Compute total weight of all validators, as well as the total weight of
  // unresponsive validators
  double totalWeight = 0.0;
  double unresponsiveWeight = 0.0;
  for (ValidatorEntry const &validator : validators) {
    double normalizedWeight = expectedNormalizedWeight(
        orgQualityCounts, maxQuality, orgQualities.at(validator.mHomeDomain),
        orgSizes.at(validator.mHomeDomain));
    totalWeight += normalizedWeight;
    if (unresponsive.count(validator.mKey)) {
      unresponsiveWeight += normalizedWeight;
    }
  }

  // Compute the average weight of an unresponsive node
  double avgUnresponsiveWeight = unresponsiveWeight / numUnresponsive;

  // Compute expected number of ledgers experiencing `n` timeouts where `n` is
  // the index of the `timeouts` vector. This vector is a mapping from number
  // of timeouts to expected number of ledgers experiencing that number of
  // timeouts.
  std::vector<int> expectedTimeouts(numUnresponsive + 1);
  double remainingWeight = totalWeight;
  int remainingUnresponsive = numUnresponsive;
  int remainingLedgers = numLedgers;
  for (int i = 0; i < expectedTimeouts.size(); ++i) {
    double timeoutProb =
        (avgUnresponsiveWeight * remainingUnresponsive) / remainingWeight;
    // To get expected number of ledgers experiencing `i` timeouts, we take
    // the probability a timeout does not occur and multiply it by the
    // number of remaining ledgers.
    int expectedLedgers = (1 - timeoutProb) * remainingLedgers;
    expectedTimeouts.at(i) = expectedLedgers;

    // Remaining ledgers decreases by expected number of ledgers
    // experiencing `i` timeouts
    remainingLedgers -= expectedLedgers;

    // For `i+1` timeouts to occur, an unresponsive node must be chosen.
    // Therefore, deduct the average weight of an unresponsive node from the
    // total weight left in the network.
    remainingWeight -= avgUnresponsiveWeight;
    --remainingUnresponsive;
  }

  // Generate a config
  Config cfg = getTestConfig();
  cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
  cfg.generateQuorumSetForTesting(validators);
  cfg.NODE_SEED = sks.front();

  // Create an application
  VirtualClock clock;
  Application::pointer app = createTestApplication(clock, cfg);

  // Run for `numLedgers` slots, recording the number of times each slot timed
  // out due to unresponsive nodes before successfully electing a responsive
  // leader.
  SCP &scp = dynamic_cast<HerderImpl &>(app->getHerder()).getSCP();
  std::vector<int> timeouts(numUnresponsive + 1);
  for (int i = 0; i < numLedgers; ++i) {
    Value const v = getRandomValue();
    auto s = std::make_shared<Slot>(i, scp);

    TestNominationProtocol np(*s);
    for (int i = 0; i < timeouts.size(); ++i) {
      std::set<NodeID> const &leaders = np.updateRoundLeadersForTesting(v);
      // If leaders is a subset of unresponsive, then a timeout occurs.
      if (!std::includes(unresponsive.begin(), unresponsive.end(),
                         leaders.begin(), leaders.end())) {
        ++timeouts.at(i);
        break;
      }
    }
  }

  // Allow for some small multiplicative increase in timeouts from the
  // theoretical value.  We're working with probabilistic simulation here so
  // we can't be too strict or the test will be flaky.
  double constexpr epsilon = 1.1;

  // There's not enough data in the tail of the distribution to allow us to
  // assert that the timeout values are what's expected. To avoid sporadic
  // test failures, we cut off `tailCutoffPoint` of the tail of the
  // distribution for the purposes of asserting test values. However, the test
  // will still log those values for manual examination.
  double constexpr tailCutoffPoint = 0.05;

  // Analyze timeouts
  int numLedgersRemaining = numLedgers;
  for (int i = 0; i < timeouts.size(); ++i) {
    int const numTimeouts = timeouts.at(i);
    int const expectedNumTimeouts = expectedTimeouts.at(i);

    if (numLedgersRemaining > numLedgers * tailCutoffPoint) {
      // Check that timeouts are less than epsilon times the expected
      // value. Also stop checking when we're at the last
      // `tailCutoffPoint` timeouts as the data is too sparse to be
      // useful.
      REQUIRE(numTimeouts < expectedNumTimeouts * epsilon);
    }
    CLOG_INFO(Herder, "Ledgers with {} timeouts: {} ({}%)", i, numTimeouts,
              numTimeouts * 100.0 / numLedgers);
    numLedgersRemaining -= numTimeouts;
  }
}

// Test timeouts for a tier 1-like topology with 1-5 unresponsive nodes. This
// test serves two purposes:
// 1. It contains assertions checking for moderate (10%) deviations from the
//    expected behavior of the nomination algorithm. These should detect any
//    major issues/regressions with the algorithm.
// 2. It logs the distributions of timeouts for manual inspection. This is
//    useful for understanding the behavior of the algorithm and for testing
//    specific scenarios one might be interested in (e.g., if 3 tier 1 nodes
//    are heavily lagging, what is the impact on nomination timeouts?).
TEST_CASE("Unresponsive quorum timeouts", "[herder]") {
  // Number of slots to run for
  int constexpr numLedgers = 20000;

  auto t = tier1Like();
  for (int i = 1; i <= 5; ++i) {
    CLOG_INFO(Herder, "Simulating nomination with {} unresponsive nodes", i);
    testUnresponsiveTimeouts(t, i, numLedgers);
  }
}

// This test checks that the parallel tx set downloading mechanism does not
// interfere with Herder's envelope validation
TEST_CASE_VERSIONS("Herder properly validates when tx set is missing",
                   "[herder]") {
  Config cfg(getTestConfig());
  cfg.MANUAL_CLOSE = false;
  cfg.EXPERIMENTAL_PARALLEL_TX_SET_DOWNLOAD = true;

  VirtualClock clock;

  auto peerKey = SecretKey::pseudoRandomForTesting();
  auto const &peerPk = peerKey.getPublicKey();
  cfg.QUORUM_SET.validators.emplace_back(peerPk);
  Application::pointer app = createTestApplication(clock, cfg);

  for_versions_from(
      static_cast<uint32_t>(EMPTY_TX_SET_PROTOCOL_VERSION), *app, [&] {
        auto const lcl = app->getLedgerManager().getLastClosedLedgerHeader();
        auto &herder = static_cast<HerderImpl &>(app->getHerder());
        auto &pendingEnvelopes = herder.getPendingEnvelopes();

        // Custom qset (single peer, threshold 1), pre-cached so the
        // envelope wouldn't be stuck waiting for the qset to arrive.
        SCPQuorumSet qSet;
        qSet.threshold = 1;
        qSet.validators.push_back(peerPk);
        auto qSetHash = sha256(xdr::xdr_to_opaque(qSet));
        pendingEnvelopes.addSCPQuorumSet(qSetHash, qSet);

        // Tx set hash deliberately fake and *not* cached.
        Hash fakeTxSetHash;
        fakeTxSetHash.fill(0xAB);

        auto makePrepareFromPeer = [&](uint64_t closeTime) {
          auto sv = herder.makeStellarValue(fakeTxSetHash, closeTime,
                                            emptyUpgradeSteps, peerKey);
          auto opaqueValue = xdr::xdr_to_opaque(sv);

          SCPEnvelope env;
          env.statement.slotIndex = lcl.header.ledgerSeq + 1;
          env.statement.pledges.type(SCP_ST_PREPARE);
          auto &prep = env.statement.pledges.prepare();
          prep.ballot.counter = 1;
          prep.ballot.value = opaqueValue;
          prep.quorumSetHash = qSetHash;
          env.statement.nodeID = peerPk;
          herder.signEnvelope(peerKey, env);
          return env;
        };

        uint64_t const badCloseTime =
            app->timeNow() + Herder::MAX_TIME_SLIP_SECONDS.count() + 60;
        auto env = makePrepareFromPeer(badCloseTime);

        // Envelope should be rejected as it has a bad close time
        REQUIRE(herder.recvSCPEnvelope(env) ==
                Herder::ENVELOPE_STATUS_DISCARDED);
      });
}
