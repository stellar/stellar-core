// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SignerKey.h"
#include "crypto/SignerKeyUtils.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "lib/catch.hpp"
#include "lib/json/json.h"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/ManageSellOfferOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SponsorshipTestUtils.h"
#include "util/Logging.h"
#include "util/Timer.h"

using namespace stellar;
using namespace stellar::txbridge;
using namespace stellar::txtest;

/*
  Tests that are testing the common envelope used in transactions.
  Things like:
    authz/authn
    double spend
*/

TEST_CASE("txset - correct apply order", "[tx][envelope]")
{
    Config cfg = getTestConfig();
    cfg.NODE_SEED = SecretKey::fromSeed(sha256("NODE_SEED"));

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);
    const int64_t paymentAmount = app->getLedgerManager().getLastReserve() * 10;

    auto a1 = root.create("a1", paymentAmount);
    auto b1 = root.create("b1", paymentAmount);
    a1.pay(b1, 1000);
    closeLedgerOn(*app, 2, 1, 1, 2016);

    auto tx1 = b1.tx({accountMerge(a1)});
    auto tx2 = a1.tx({b1.op(payment(root, 110)), root.op(payment(a1, 101))});

    auto txSet = std::make_shared<TxSetFrame>(
        app->getLedgerManager().getLastClosedLedgerHeader().hash);
    txSet->add(tx1);
    txSet->add(tx2);

    // Sort for apply re-orders transaction set
    auto txs = txSet->sortForApply();
    REQUIRE(txs.size() == 2);
    REQUIRE(txs[1]->getFullHash() == tx1->getFullHash());
    REQUIRE(txs[0]->getFullHash() == tx2->getFullHash());
}

TEST_CASE("txenvelope", "[tx][envelope]")
{
    Config cfg = getTestConfig();

    // Do our setup in version 1 so that for_all_versions below does not
    // try to downgrade us from >1 to 1.
    cfg.USE_CONFIG_FOR_GENESIS = false;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);

    const int64_t paymentAmount = app->getLedgerManager().getLastReserve() * 10;

    SECTION("outer envelope")
    {
        auto a1 = TestAccount{*app, getAccount("A")};
        SECTION("no signature")
        {
            for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                auto txFrame =
                    root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
                getSignatures(txFrame).clear();

                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                auto txFrame =
                    root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
                getSignatures(txFrame).clear();

                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txSUCCESS);
            });
        }

        SECTION("bad signature")
        {
            for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                auto txFrame =
                    root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
                getSignatures(txFrame)[0].signature = Signature(32, 123);

                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                auto txFrame =
                    root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
                getSignatures(txFrame)[0].signature = Signature(32, 123);

                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txSUCCESS);
            });
        }

        SECTION("bad signature (wrong hint)")
        {
            for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                auto txFrame =
                    root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
                getSignatures(txFrame)[0].hint.fill(1);

                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                auto txFrame =
                    root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
                getSignatures(txFrame)[0].hint.fill(1);

                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txSUCCESS);
            });
        }

        SECTION("too many signatures (signed twice)")
        {
            for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                auto txFrame =
                    root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
                txFrame->addSignature(a1);

                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txBAD_AUTH_EXTRA);
            });
            for_versions({7}, *app, [&] {
                auto txFrame =
                    root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
                txFrame->addSignature(a1);

                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txSUCCESS);
            });
        }

        SECTION("too many signatures (unused signature)")
        {
            for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                auto txFrame =
                    root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
                SecretKey bogus = getAccount("bogus");
                txFrame->addSignature(bogus);

                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txBAD_AUTH_EXTRA);
            });
            for_versions({7}, *app, [&] {
                auto txFrame =
                    root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
                SecretKey bogus = getAccount("bogus");
                txFrame->addSignature(bogus);

                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txSUCCESS);
            });
        }
    }

    SECTION("multisig")
    {
        auto a1 = root.create("A", paymentAmount);
        auto s1 = getAccount("S1");
        auto sk1 = makeSigner(s1, 5);
        auto th = setMasterWeight(100) | setLowThreshold(10) |
                  setMedThreshold(50) | setHighThreshold(100);

        a1.setOptions(th | setSigner(sk1));

        auto s2 = getAccount("S2");
        auto sk2 = makeSigner(s2, 95); // med rights account

        a1.setOptions(setSigner(sk2));

        SECTION("not enough rights (envelope)")
        {
            for_all_versions_except({7}, *app, [&] {
                auto tx = a1.tx({payment(root, 1000)});

                // only sign with s1
                getSignatures(tx).clear();
                tx->addSignature(s1);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                auto tx = a1.tx({payment(root, 1000)});

                // only sign with s1
                getSignatures(tx).clear();
                tx->addSignature(s1);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
            });
        }

        SECTION("not enough rights (operation, together)")
        {
            for_all_versions_except({7}, *app, [&] {
                // updating thresholds requires high
                auto tx = a1.tx({setOptions(th | setSigner(sk1))});

                // only sign with s2 (med)
                getSignatures(tx).clear();
                tx->addSignature(s2);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txFAILED);
                REQUIRE(getFirstResultCode(*tx) == opBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                // updating thresholds requires high
                auto tx = a1.tx({setOptions(th | setSigner(sk1))});

                // only sign with s2 (med)
                getSignatures(tx).clear();
                tx->addSignature(s2);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
            });
        }

        SECTION("not enough rights (first thresholds)")
        {
            for_all_versions_except({7}, *app, [&] {
                // updating thresholds requires high
                auto tx = a1.tx({setOptions(th), setOptions(setSigner(sk1))});

                // only sign with s2 (med)
                getSignatures(tx).clear();
                tx->addSignature(s2);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txFAILED);
                REQUIRE(getFirstResultCode(*tx) == opBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                // updating thresholds requires high
                auto tx = a1.tx({setOptions(th), setOptions(setSigner(sk1))});

                // only sign with s2 (med)
                getSignatures(tx).clear();
                tx->addSignature(s2);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
            });
        }

        SECTION("not enough rights (first signer)")
        {
            for_all_versions_except({7}, *app, [&] {
                // updating thresholds requires high
                auto tx = a1.tx({setOptions(setSigner(sk1)), setOptions(th)});

                // only sign with s2 (med)
                getSignatures(tx).clear();
                tx->addSignature(s2);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txFAILED);
                REQUIRE(getFirstResultCode(*tx) == opBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                // updating thresholds requires high
                auto tx = a1.tx({setOptions(setSigner(sk1)), setOptions(th)});

                // only sign with s2 (med)
                getSignatures(tx).clear();
                tx->addSignature(s2);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
            });
        }

        SECTION("success two signatures, together")
        {
            for_all_versions(*app, [&] {
                // updating thresholds requires high
                auto tx = a1.tx({setOptions(th | setSigner(sk1))});

                getSignatures(tx).clear();
                tx->addSignature(s1);
                tx->addSignature(s2);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
                REQUIRE(SetOptionsOpFrame::getInnerCode(getFirstResult(*tx)) ==
                        SET_OPTIONS_SUCCESS);
            });
        }

        SECTION("success two signatures, first thresholds")
        {
            for_all_versions(*app, [&] {
                // updating thresholds requires high
                auto tx = a1.tx({setOptions(th), setOptions(setSigner(sk1))});

                getSignatures(tx).clear();
                tx->addSignature(s1);
                tx->addSignature(s2);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
                REQUIRE(SetOptionsOpFrame::getInnerCode(getFirstResult(*tx)) ==
                        SET_OPTIONS_SUCCESS);
            });
        }

        SECTION("success two signatures, first signer")
        {
            for_all_versions(*app, [&] {
                // updating thresholds requires high
                auto tx = a1.tx({setOptions(setSigner(sk1)), setOptions(th)});

                getSignatures(tx).clear();
                tx->addSignature(s1);
                tx->addSignature(s2);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
                REQUIRE(SetOptionsOpFrame::getInnerCode(getFirstResult(*tx)) ==
                        SET_OPTIONS_SUCCESS);
            });
        }

        SECTION("without master key")
        {
            a1.setOptions(setMasterWeight(0));

            auto checkPayment = [&](bool withMaster,
                                    TransactionResultCode expectedRes) {
                for_all_versions_except({7}, *app, [&] {
                    // payment requires medium
                    auto tx = a1.tx({payment(root, 1000)});
                    // only sign with s2 (med)
                    if (!withMaster)
                    {
                        getSignatures(tx).clear();
                    }
                    tx->addSignature(s2);

                    applyCheck(tx, *app);
                    REQUIRE(tx->getResultCode() == expectedRes);
                });
            };

            SECTION("good tx")
            {
                checkPayment(false, txSUCCESS);
            }
            SECTION("master key is extra")
            {
                checkPayment(true, txBAD_AUTH_EXTRA);
            }
        }

        SECTION("account locked down")
        {
            root.setOptions(setMasterWeight(0));

            for_versions_from(8, *app, [&] {
                REQUIRE_THROWS_AS(root.pay(root, 1000), ex_txBAD_AUTH);
            });
        }

        SECTION("do not allow duplicate signature")
        {
            for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                auto tx = a1.tx({payment(root, 1000)});

                getSignatures(tx).clear();
                for (auto i = 0; i < 10; i++)
                    tx->addSignature(s1);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                auto tx = a1.tx({payment(root, 1000)});

                getSignatures(tx).clear();
                for (auto i = 0; i < 10; i++)
                    tx->addSignature(s1);

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
            });
        }
    }

    SECTION("alternative signatures")
    {
        auto a1 = root.create("A", paymentAmount);

        struct AltSignature
        {
            std::string name;
            bool autoRemove;
            std::function<SignerKey(TransactionFrame&)> createSigner;
            std::function<void(TransactionFrame&)> sign;
        };

        // ensue that hash(x) supports 0 inside 'x'
        auto x = std::vector<uint8_t>{'a', 'b', 'c', 0,   'd', 'e', 'f', 0,
                                      0,   0,   'g', 'h', 'i', 'j', 'k', 'l',
                                      'A', 'B', 'C', 0,   'D', 'E', 'F', 0,
                                      0,   0,   'G', 'H', 'I', 'J', 'K', 'L'};
        auto alternatives = std::vector<AltSignature>{
            AltSignature{"hash tx", true,
                         [](TransactionFrame& tx) {
                             tx.clearCached();
                             return SignerKeyUtils::preAuthTxKey(tx);
                         },
                         [](TransactionFrame&) {}},
            AltSignature{
                "hash x", false,
                [x](TransactionFrame&) { return SignerKeyUtils::hashXKey(x); },
                [x](TransactionFrame& tx) {
                    tx.addSignature(SignatureUtils::signHashX(x));
                }}};

        for (auto const& alternative : alternatives)
        {
            SECTION(alternative.name)
            {
                for_versions_to(2, *app, [&] {
                    auto tx = a1.tx({payment(root, 1000)});
                    getSignatures(tx).clear();
                    setSeqNum(tx, tx->getSeqNum() + 1);
                    a1.setSequenceNumber(a1.getLastSequenceNumber() - 1);

                    SignerKey sk = alternative.createSigner(*tx);
                    Signer sk1(sk, 1);
                    REQUIRE_THROWS_AS(a1.setOptions(setSigner(sk1)),
                                      ex_SET_OPTIONS_BAD_SIGNER);
                });

                SECTION("single signature")
                {
                    SECTION("invalid seq nr")
                    {
                        TransactionFramePtr tx;
                        auto setup = [&]() {
                            tx = a1.tx({payment(root, 1000)});
                            a1.setSequenceNumber(a1.getLastSequenceNumber() -
                                                 1);
                            getSignatures(tx).clear();

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 1);
                            a1.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                            alternative.sign(*tx);
                        };
                        for_versions(3, 9, *app, [&] {
                            setup();
                            {
                                LedgerTxn ltx(app->getLedgerTxnRoot());
                                REQUIRE(!tx->checkValid(ltx, 0, 0, 0));
                            }
                            REQUIRE(tx->getResultCode() == txBAD_SEQ);
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                        });
                        for_versions(10, 12, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_SEQ);
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                        });
                        for_versions_from(13, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_SEQ);
                            REQUIRE(getAccountSigners(a1, *app).size() ==
                                    (alternative.autoRemove ? 0 : 1));
                        });
                    }

                    SECTION("invalid signature")
                    {
                        TransactionFramePtr tx;
                        auto setup = [&]() {
                            tx = a1.tx({payment(root, 1000)});
                            getSignatures(tx).clear();
                            setSeqNum(tx, tx->getSeqNum() + 1);
                            a1.setSequenceNumber(a1.getLastSequenceNumber() -
                                                 1);

                            SignerKey sk = alternative.createSigner(*tx);
                            KeyFunctions<SignerKey>::getKeyValue(sk)[0] ^= 0x01;
                            Signer sk1(sk, 1);
                            a1.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                            alternative.sign(*tx);
                        };
                        for_versions_from({3, 4, 5, 6, 8}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH);
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                        });
                        for_versions({7}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                        });
                    }

                    SECTION("too many signatures (signed by owner)")
                    {
                        TransactionFramePtr tx;
                        auto setup = [&]() {
                            tx = a1.tx({payment(root, 1000)});
                            setSeqNum(tx, tx->getSeqNum() + 1);
                            a1.setSequenceNumber(a1.getLastSequenceNumber() -
                                                 1);

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 1);
                            a1.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                            alternative.sign(*tx);
                        };
                        for_versions({3, 4, 5, 6, 8, 9}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH_EXTRA);
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                        });
                        for_versions({7}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                        });
                        for_versions_from(10, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH_EXTRA);
                            REQUIRE(getAccountSigners(a1, *app).size() ==
                                    (alternative.autoRemove ? 0 : 1));
                        });
                    }

                    SECTION("success")
                    {
                        TransactionFramePtr tx;
                        auto setup = [&]() {
                            tx = a1.tx({payment(root, 1000)});
                            getSignatures(tx).clear();
                            setSeqNum(tx, tx->getSeqNum() + 1);
                            a1.setSequenceNumber(a1.getLastSequenceNumber() -
                                                 1);

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 1);
                            a1.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                            alternative.sign(*tx);
                        };
                        for_versions_from({3, 4, 5, 6, 8}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(
                                        *tx)) == PAYMENT_SUCCESS);
                            REQUIRE(getAccountSigners(a1, *app).size() ==
                                    (alternative.autoRemove ? 0 : 1));
                        });
                        for_versions({7}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(
                                        *tx)) == PAYMENT_SUCCESS);
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                        });
                    }

                    SECTION("merge source account before payment")
                    {
                        auto b1 = root.create("b1", paymentAmount);
                        a1.pay(b1, 1000);

                        closeLedgerOn(*app, 2, 1, 1, 2016);

                        auto runTest = [&](bool txAccountMissing) {
                            // Create merge tx
                            auto txMerge = b1.tx({accountMerge(a1)});

                            // Create payment tx. Depending on txAccountMissing,
                            // either the transaction source account or the
                            // operation source account will go missing (because
                            // b1 will be merged into a1)
                            auto& txAccount = txAccountMissing ? b1 : a1;
                            auto& opAccount = txAccountMissing ? a1 : b1;
                            auto tx =
                                txAccount.tx({opAccount.op(payment(root, 110)),
                                              root.op(payment(a1, 101))});
                            getSignatures(tx).clear();

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 1);
                            alternative.sign(*tx);

                            // Create signer tx
                            auto setSignerTx =
                                root.tx({setOptions(setSigner(sk1)),
                                         a1.op(setOptions(setSigner(sk1))),
                                         b1.op(setOptions(setSigner(sk1)))});
                            setSignerTx->addSignature(a1.getSecretKey());
                            setSignerTx->addSignature(b1.getSecretKey());

                            // Apply signer tx
                            applyCheck(setSignerTx, *app);
                            REQUIRE(setSignerTx->getResultCode() == txSUCCESS);

                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                            REQUIRE(getAccountSigners(b1, *app).size() == 1);
                            REQUIRE(getAccountSigners(root, *app).size() == 1);

                            // merge b1 into a1 and attempt the payment tx
                            auto r = closeLedgerOn(*app, 3, 1, 2, 2016,
                                                   {txMerge, tx},
                                                   /* strictOrder */ true);

                            if (txAccountMissing)
                            {
                                checkTx(0, r, txSUCCESS);
                                checkTx(1, r, txNO_ACCOUNT);
                            }
                            else
                            {
                                checkTx(0, r, txSUCCESS);
                                checkTx(1, r, txFAILED);
                            }

                            uint32_t ledgerVersion;
                            {
                                LedgerTxn ltx(app->getLedgerTxnRoot());
                                ledgerVersion =
                                    ltx.loadHeader().current().ledgerVersion;
                            }

                            // If the operation source account is missing, then
                            // the signatures can be removed if V10 or greater.
                            // However, if the transaction source account is
                            // missing, then signatures can only be removed if
                            // V13 or greater.
                            if (ledgerVersion < 13 &&
                                (txAccountMissing || ledgerVersion < 10))
                            {
                                REQUIRE(getAccountSigners(a1, *app).size() ==
                                        1);
                                REQUIRE(getAccountSigners(root, *app).size() ==
                                        1);
                            }
                            else
                            {
                                REQUIRE(getAccountSigners(a1, *app).size() ==
                                        (alternative.autoRemove ? 0 : 1));
                                REQUIRE(getAccountSigners(root, *app).size() ==
                                        (alternative.autoRemove ? 0 : 1));
                            }
                        };

                        SECTION("merge op source account")
                        {
                            for_versions_from(3, *app, [&] { runTest(false); });
                        }

                        SECTION("merge tx source account")
                        {
                            for_versions_from(3, *app, [&] { runTest(true); });
                        }
                    }

                    SECTION("accountMerge signing account")
                    {
                        auto b1 = root.create("b1", paymentAmount);
                        a1.pay(b1, 1000);

                        closeLedgerOn(*app, 2, 1, 1, 2016);

                        for_versions_from(3, *app, [&] {
                            auto tx = b1.tx({accountMerge(a1)},
                                            b1.getLastSequenceNumber() + 2);
                            getSignatures(tx).clear();

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 1);
                            b1.setOptions(setSigner(sk1));

                            REQUIRE(getAccountSigners(a1, *app).size() == 0);
                            REQUIRE(getAccountSigners(b1, *app).size() == 1);
                            alternative.sign(*tx);

                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(MergeOpFrame::getInnerCode(getFirstResult(
                                        *tx)) == ACCOUNT_MERGE_SUCCESS);
                            REQUIRE(getAccountSigners(a1, *app).size() == 0);

                            {
                                LedgerTxn ltx(app->getLedgerTxnRoot());
                                REQUIRE(!stellar::loadAccount(ltx, b1));
                            }
                        });
                    }

                    SECTION("failing transaction")
                    {
                        TransactionFramePtr tx;
                        auto setup = [&]() {
                            tx = a1.tx({payment(root, -1)});
                            getSignatures(tx).clear();
                            setSeqNum(tx, tx->getSeqNum() + 1);
                            a1.setSequenceNumber(a1.getLastSequenceNumber() -
                                                 1);

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 1);
                            a1.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                            alternative.sign(*tx);
                        };
                        for_versions(3, 9, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == stellar::txFAILED);
                            REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(
                                        *tx)) == stellar::PAYMENT_MALFORMED);
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                        });
                        for_versions_from(10, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == stellar::txFAILED);
                            REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(
                                        *tx)) == stellar::PAYMENT_MALFORMED);
                            REQUIRE(getAccountSigners(a1, *app).size() ==
                                    (alternative.autoRemove ? 0 : 1));
                        });
                    }
                }

                SECTION("multisig")
                {
                    auto s1 = getAccount("S1");
                    auto sk1Org = makeSigner(s1, 95);
                    auto th = setMasterWeight(100) | setLowThreshold(10) |
                              setMedThreshold(50) | setHighThreshold(100);

                    a1.setOptions(th | setSigner(sk1Org));

                    SECTION("not enough rights (envelope)")
                    {
                        TransactionFramePtr tx;
                        auto setup = [&]() {
                            tx = a1.tx({payment(root, 1000)});
                            getSignatures(tx).clear();
                            setSeqNum(tx, tx->getSeqNum() + 1);
                            a1.setSequenceNumber(a1.getLastSequenceNumber() -
                                                 1);

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 5); // below low rights
                            a1.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                            alternative.sign(*tx);
                        };
                        for_versions({3, 4, 5, 6, 8, 9, 10, 11, 12}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH);
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                        });
                        for_versions_from(13, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH);
                            REQUIRE(getAccountSigners(a1, *app).size() ==
                                    (alternative.autoRemove ? 1 : 2));
                        });
                        for_versions({7}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                        });
                    }

                    SECTION("not enough rights (envelope). Same pre auth "
                            "signer on both tx and op source account")
                    {
                        TransactionFramePtr tx;
                        auto setup = [&]() {
                            tx = a1.tx({root.op(payment(a1, 100))});
                            getSignatures(tx).clear();
                            setSeqNum(tx, tx->getSeqNum() + 1);
                            a1.setSequenceNumber(a1.getLastSequenceNumber() -
                                                 1);

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 5); // below low rights
                            a1.setOptions(setSigner(sk1));
                            root.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                            REQUIRE(getAccountSigners(root, *app).size() == 1);
                            alternative.sign(*tx);
                        };
                        for_versions({3, 4, 5, 6, 8, 9, 10, 11, 12}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH);
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                            REQUIRE(getAccountSigners(root, *app).size() == 1);
                        });
                        for_versions_from(13, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH);
                            REQUIRE(getAccountSigners(a1, *app).size() ==
                                    (alternative.autoRemove ? 1 : 2));
                            REQUIRE(getAccountSigners(root, *app).size() ==
                                    (alternative.autoRemove ? 0 : 1));
                        });
                        for_versions({7}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                            REQUIRE(getAccountSigners(root, *app).size() == 1);
                        });
                    }

                    SECTION("Bad seq num. Same pre auth "
                            "signer on both tx and op source account")
                    {
                        TransactionFramePtr tx;
                        auto setup = [&]() {
                            tx = a1.tx({root.op(payment(a1, 100))});
                            getSignatures(tx).clear();
                            a1.setSequenceNumber(a1.getLastSequenceNumber() -
                                                 1);

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 5); // below low rights
                            a1.setOptions(setSigner(sk1));
                            root.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                            REQUIRE(getAccountSigners(root, *app).size() == 1);
                            alternative.sign(*tx);
                        };
                        for_versions(10, 12, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_SEQ);
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                            REQUIRE(getAccountSigners(root, *app).size() == 1);
                        });
                        for_versions_from(13, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_SEQ);
                            REQUIRE(getAccountSigners(a1, *app).size() ==
                                    (alternative.autoRemove ? 1 : 2));
                            REQUIRE(getAccountSigners(root, *app).size() ==
                                    (alternative.autoRemove ? 0 : 1));
                        });
                    }

                    SECTION("not enough rights (operation)")
                    {
                        // updating thresholds requires high
                        TransactionFramePtr tx;
                        auto setup = [&]() {
                            tx = a1.tx({setOptions(th)},
                                       a1.getLastSequenceNumber() + 2);
                            getSignatures(tx).clear();

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 95); // med rights account
                            a1.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                            alternative.sign(*tx);
                        };
                        for_versions({3, 4, 5, 6, 8, 9}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txFAILED);
                            REQUIRE(getFirstResultCode(*tx) == opBAD_AUTH);
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                        });
                        for_versions({7}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                        });
                        for_versions_from(10, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txFAILED);
                            REQUIRE(getFirstResultCode(*tx) == opBAD_AUTH);
                            REQUIRE(getAccountSigners(a1, *app).size() ==
                                    (alternative.autoRemove ? 1 : 2));
                        });
                    }

                    SECTION("not enough rights on first operation")
                    {
                        TransactionFramePtr tx1, tx2;
                        auto setup = [&]() {
                            auto b = root.create("b", 1000000000);
                            tx1 = b.tx({setOptions(
                                setMasterWeight(1) | setLowThreshold(1) |
                                setMedThreshold(2) | setHighThreshold(3))});
                            tx2 = b.tx(
                                {payment(root, 100), root.op(payment(b, 100))},
                                b.getLastSequenceNumber() + 1);

                            SignerKey sk = alternative.createSigner(*tx2);
                            Signer sk1(sk, 100); // high rights account
                            root.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(root, *app).size() == 1);
                            alternative.sign(*tx2);
                        };
                        for_versions(3, 9, *app, [&] {
                            setup();
                            closeLedgerOn(*app, 2, 1, 1, 2010, {tx1, tx2});
                            REQUIRE(getAccountSigners(root, *app).size() == 1);
                        });
                        for_versions_from(10, *app, [&] {
                            setup();
                            closeLedgerOn(*app, 2, 1, 1, 2010, {tx1, tx2});
                            REQUIRE(getAccountSigners(root, *app).size() ==
                                    (alternative.autoRemove ? 0 : 1));
                        });
                    }

                    SECTION("not enough rights on second operation")
                    {
                        TransactionFramePtr tx1, tx2;
                        auto setup = [&]() {
                            auto b = root.create("b", 1000000000);
                            tx1 = b.tx({setOptions(
                                setMasterWeight(1) | setLowThreshold(1) |
                                setMedThreshold(2) | setHighThreshold(3))});
                            tx2 = b.tx(
                                {root.op(payment(b, 100)), payment(root, 100)},
                                b.getLastSequenceNumber() + 1);

                            SignerKey sk = alternative.createSigner(*tx2);
                            Signer sk1(sk, 100); // high rights account
                            root.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(root, *app).size() == 1);
                            alternative.sign(*tx2);
                        };
                        for_versions(3, 9, *app, [&] {
                            setup();
                            closeLedgerOn(*app, 2, 1, 1, 2010, {tx1, tx2});
                            REQUIRE(getAccountSigners(root, *app).size() == 1);
                        });
                        for_versions_from(10, *app, [&] {
                            setup();
                            closeLedgerOn(*app, 2, 1, 1, 2010, {tx1, tx2});
                            REQUIRE(getAccountSigners(root, *app).size() ==
                                    (alternative.autoRemove ? 0 : 1));
                        });
                    }

                    SECTION("signatures removed from multiple accounts even "
                            "though transaction failed")
                    {
                        TransactionFramePtr tx;
                        auto setup = [&]() {
                            tx = a1.tx({root.op(payment(a1, 100))});
                            getSignatures(tx).clear();

                            setSeqNum(tx, tx->getSeqNum() + 1);
                            // add signer twice.
                            tx->addSignature(s1);
                            tx->addSignature(s1);
                            a1.setSequenceNumber(a1.getLastSequenceNumber() -
                                                 1);

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 1);
                            a1.setOptions(setSigner(sk1));
                            root.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                            REQUIRE(getAccountSigners(root, *app).size() == 1);
                            alternative.sign(*tx);
                        };
                        for_versions({9}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH_EXTRA);
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                            REQUIRE(getAccountSigners(root, *app).size() == 1);
                        });

                        for_versions({10}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH_EXTRA);
                            REQUIRE(getAccountSigners(a1, *app).size() ==
                                    (alternative.autoRemove ? 1 : 2));
                            REQUIRE(getAccountSigners(root, *app).size() ==
                                    (alternative.autoRemove ? 0 : 1));
                        });
                    }

                    SECTION("success complex sponsored signatures + " +
                            alternative.name)
                    {
                        for_versions_from(14, *app, [&] {
                            auto a2 = root.create("A2", paymentAmount);

                            TransactionFramePtr tx;
                            tx = a1.tx({payment(root, 1000)});
                            getSignatures(tx).clear();
                            tx->addSignature(s1);

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 5); // below low rights

                            // add two more signers. We want to sandwich the
                            // signer that will be removed (sk1), so we can
                            // verify how signerSponsoringIDs changes
                            Signer signer1 = makeSigner(getAccount("1"), 5);
                            Signer signer2(SignerKeyUtils::hashXKey("5"), 5);

                            REQUIRE(signer1.key < sk);
                            REQUIRE(sk < signer2.key);

                            // sk1 is sponsored by a2, while signer1 and signer2
                            // are sponsored by root
                            auto insideSignerTx = transactionFrameFromOps(
                                app->getNetworkID(), a2,
                                {a2.op(beginSponsoringFutureReserves(a1)),
                                 a1.op(setOptions(setSigner(sk1))),
                                 a1.op(endSponsoringFutureReserves())},
                                {a1});
                            {
                                LedgerTxn ltx(app->getLedgerTxnRoot());
                                TransactionMeta txm(2);
                                REQUIRE(
                                    insideSignerTx->checkValid(ltx, 0, 0, 0));
                                REQUIRE(insideSignerTx->apply(*app, ltx, txm));
                                REQUIRE(insideSignerTx->getResultCode() ==
                                        txSUCCESS);
                                ltx.commit();
                            }

                            auto outsideSignerTx = transactionFrameFromOps(
                                app->getNetworkID(), root,
                                {root.op(beginSponsoringFutureReserves(a1)),
                                 a1.op(setOptions(setSigner(signer1))),
                                 a1.op(setOptions(setSigner(signer2))),
                                 a1.op(endSponsoringFutureReserves())},
                                {a1});
                            {
                                LedgerTxn ltx(app->getLedgerTxnRoot());
                                TransactionMeta txm(2);
                                REQUIRE(
                                    outsideSignerTx->checkValid(ltx, 0, 0, 0));
                                REQUIRE(outsideSignerTx->apply(*app, ltx, txm));
                                REQUIRE(outsideSignerTx->getResultCode() ==
                                        txSUCCESS);
                                ltx.commit();
                            }

                            {
                                LedgerTxn ltx(app->getLedgerTxnRoot());
                                checkSponsorship(ltx, a1.getPublicKey(),
                                                 signer1.key, 2,
                                                 &root.getPublicKey());
                                checkSponsorship(ltx, a1.getPublicKey(),
                                                 signer2.key, 2,
                                                 &root.getPublicKey());
                                checkSponsorship(ltx, a1.getPublicKey(), sk, 2,
                                                 &a2.getPublicKey());

                                checkSponsorship(ltx, root.getPublicKey(), 0,
                                                 nullptr, 0, 2, 2, 0);
                                checkSponsorship(ltx, a2.getPublicKey(), 0,
                                                 nullptr, 0, 2, 1, 0);
                            }

                            REQUIRE(getAccountSigners(a1, *app).size() == 4);
                            alternative.sign(*tx);

                            applyTx(tx, *app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(
                                        *tx)) == PAYMENT_SUCCESS);
                            REQUIRE(getAccountSigners(a1, *app).size() ==
                                    (alternative.autoRemove ? 3 : 4));

                            if (alternative.autoRemove)
                            {
                                LedgerTxn ltx(app->getLedgerTxnRoot());
                                checkSponsorship(ltx, root.getPublicKey(), 0,
                                                 nullptr, 0, 2, 2, 0);
                                checkSponsorship(ltx, a2.getPublicKey(), 0,
                                                 nullptr, 0, 2, 0, 0);

                                auto ltxe = stellar::loadAccount(ltx, a1);
                                auto const& a1Entry =
                                    ltxe.current().data.account();
                                auto const& sponsoringIDs =
                                    a1Entry.ext.v1()
                                        .ext.v2()
                                        .signerSponsoringIDs;

                                REQUIRE(sponsoringIDs.size() == 3);
                                REQUIRE(!sponsoringIDs[0]);
                                REQUIRE((sponsoringIDs[1] && sponsoringIDs[2]));
                                REQUIRE(*sponsoringIDs[1] ==
                                        root.getPublicKey());
                                REQUIRE(*sponsoringIDs[1] == *sponsoringIDs[2]);
                            }
                        });
                    }

                    SECTION("success signature + " + alternative.name)
                    {
                        TransactionFramePtr tx;
                        auto setup = [&]() {
                            tx = a1.tx({payment(root, 1000)});
                            getSignatures(tx).clear();
                            setSeqNum(tx, tx->getSeqNum() + 1);
                            tx->addSignature(s1);
                            a1.setSequenceNumber(a1.getLastSequenceNumber() -
                                                 1);

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 5); // below low rights
                            a1.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                            alternative.sign(*tx);
                        };
                        for_versions_from({3, 4, 5, 6, 8}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(
                                        *tx)) == PAYMENT_SUCCESS);
                            REQUIRE(getAccountSigners(a1, *app).size() ==
                                    (alternative.autoRemove ? 1 : 2));
                        });
                        for_versions({7}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(
                                        *tx)) == PAYMENT_SUCCESS);
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                        });
                    }
                }

                SECTION(alternative.name + " in op source account signers")
                {
                    TransactionFramePtr tx;
                    auto setup = [&]() {
                        auto op = a1.op(payment(root, 100));
                        tx = transactionFromOperations(
                            *app, root, root.getLastSequenceNumber() + 2, {op});
                        getSignatures(tx).clear();

                        SignerKey sk = alternative.createSigner(*tx);
                        Signer sk1(sk, 1);
                        root.setOptions(setSigner(sk1));
                        a1.setOptions(setSigner(sk1));
                        REQUIRE(getAccountSigners(root, *app).size() == 1);
                        REQUIRE(getAccountSigners(a1, *app).size() == 1);
                        alternative.sign(*tx);
                    };
                    for_versions_from({3, 4, 5, 6, 8}, *app, [&] {
                        setup();
                        applyCheck(tx, *app);
                        REQUIRE(tx->getResultCode() == txSUCCESS);
                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    getFirstResult(*tx)) == PAYMENT_SUCCESS);
                        REQUIRE(getAccountSigners(root, *app).size() ==
                                (alternative.autoRemove ? 0 : 1));
                        REQUIRE(getAccountSigners(a1, *app).size() ==
                                (alternative.autoRemove ? 0 : 1));
                    });
                    for_versions({7}, *app, [&] {
                        setup();
                        applyCheck(tx, *app);
                        REQUIRE(tx->getResultCode() == txSUCCESS);
                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    getFirstResult(*tx)) == PAYMENT_SUCCESS);
                        REQUIRE(getAccountSigners(root, *app).size() == 1);
                        REQUIRE(getAccountSigners(a1, *app).size() == 1);
                    });
                }

                SECTION(alternative.name +
                        " in multiple ops source account signers")
                {
                    TransactionFramePtr tx;
                    auto setup = [&]() {
                        auto op = a1.op(payment(root, 100));
                        tx = transactionFromOperations(
                            *app, root, root.getLastSequenceNumber() + 2,
                            {op, op});
                        getSignatures(tx).clear();

                        SignerKey sk = alternative.createSigner(*tx);
                        Signer sk1(sk, 1);
                        root.setOptions(setSigner(sk1));
                        a1.setOptions(setSigner(sk1));
                        REQUIRE(getAccountSigners(root, *app).size() == 1);
                        REQUIRE(getAccountSigners(a1, *app).size() == 1);
                        alternative.sign(*tx);
                    };
                    for_versions_from({3, 4, 5, 6, 8}, *app, [&] {
                        setup();
                        applyCheck(tx, *app);
                        REQUIRE(tx->getResultCode() == txSUCCESS);
                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    getFirstResult(*tx)) == PAYMENT_SUCCESS);
                        REQUIRE(getAccountSigners(root, *app).size() ==
                                (alternative.autoRemove ? 0 : 1));
                        REQUIRE(getAccountSigners(a1, *app).size() ==
                                (alternative.autoRemove ? 0 : 1));
                    });
                    for_versions({7}, *app, [&] {
                        setup();
                        applyCheck(tx, *app);
                        REQUIRE(tx->getResultCode() == txSUCCESS);
                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    getFirstResult(*tx)) == PAYMENT_SUCCESS);
                        REQUIRE(getAccountSigners(root, *app).size() == 1);
                        REQUIRE(getAccountSigners(a1, *app).size() == 1);
                    });
                }
            }
        }

        SECTION("empty X")
        {
            auto s1 = getAccount("S1");
            auto sk1 = makeSigner(s1, 95);
            auto th = setMasterWeight(100) | setLowThreshold(10) |
                      setMedThreshold(50) | setHighThreshold(100);

            a1.setOptions(th | setSigner(sk1));

            for_versions_from(3, *app, [&] {
                auto tx = a1.tx({payment(root, 1000)});
                getSignatures(tx).clear();

                setSeqNum(tx, tx->getSeqNum() + 1);
                tx->addSignature(s1);
                a1.setSequenceNumber(a1.getLastSequenceNumber() - 1);

                SignerKey sk = SignerKeyUtils::hashXKey(x);
                Signer sk2(sk, 5); // below low rights
                a1.setOptions(setSigner(sk2));

                REQUIRE(getAccountSigners(a1, *app).size() == 2);
                tx->addSignature(SignatureUtils::signHashX(x));

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
                REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(*tx)) ==
                        PAYMENT_SUCCESS);
                REQUIRE(getAccountSigners(a1, *app).size() == 2);
            });
        }
    }

    SECTION("batching")
    {
        SECTION("empty batch")
        {
            for_all_versions(*app, [&] {
                auto tx = root.tx({});
                setFee(tx, 1000);

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(!tx->checkValid(ltx, 0, 0, 0));
                }

                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txMISSING_OPERATION);
            });
        }

        SECTION("non empty")
        {
            auto a1 = root.create("A", paymentAmount);
            auto b1 = root.create("B", paymentAmount);

            SECTION("single tx wrapped by different account")
            {
                SECTION("missing signature")
                {
                    for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                        auto tx = a1.tx({b1.op(payment(root, 1000))});
                        getSignatures(tx).clear();
                        tx->addSignature(a1);

                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(!tx->checkValid(ltx, 0, 0, 0));
                        }
                        applyCheck(tx, *app);
                        REQUIRE(tx->getResultCode() == txFAILED);
                        REQUIRE(tx->getOperations()[0]->getResultCode() ==
                                opBAD_AUTH);
                    });
                    for_versions({7}, *app, [&] {
                        auto tx = a1.tx({b1.op(payment(root, 1000))});
                        getSignatures(tx).clear();
                        tx->addSignature(a1);

                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                        }
                        applyCheck(tx, *app);
                        REQUIRE(tx->getResultCode() == txSUCCESS);
                    });
                }

                SECTION("success")
                {
                    for_all_versions(*app, [&] {
                        auto tx = a1.tx({b1.op(payment(root, 1000))});
                        getSignatures(tx).clear();
                        tx->addSignature(a1);
                        tx->addSignature(b1);

                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                        }
                        applyCheck(tx, *app);
                        REQUIRE(tx->getResultCode() == txSUCCESS);
                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    getFirstResult(*tx)) == PAYMENT_SUCCESS);
                    });
                }
            }
            SECTION("multiple tx")
            {
                for_all_versions(*app, [&] {
                    SECTION("one invalid tx")
                    {
                        auto idr = b1.asset("IDR");
                        Price price(1, 1);
                        auto tx = a1.tx(
                            {payment(root, 1000),
                             b1.op(manageOffer(0, idr, idr, price, 1000))});
                        getSignatures(tx).clear();
                        tx->addSignature(a1);
                        tx->addSignature(b1);

                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(!tx->checkValid(ltx, 0, 0, 0));
                        }

                        applyCheck(tx, *app);

                        REQUIRE(tx->getResult().feeCharged ==
                                2 * app->getLedgerManager().getLastTxFee());
                        REQUIRE(tx->getResultCode() == txFAILED);
                        // first operation was success
                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    getFirstResult(*tx)) == PAYMENT_SUCCESS);
                        // second
                        REQUIRE(ManageSellOfferOpFrame::getInnerCode(
                                    tx->getOperations()[1]->getResult()) ==
                                MANAGE_SELL_OFFER_MALFORMED);
                    }
                    SECTION("one failed tx")
                    {
                        // this payment is too large
                        auto tx = a1.tx({payment(root, 1000),
                                         b1.op(payment(root, paymentAmount))});
                        getSignatures(tx).clear();
                        tx->addSignature(a1);
                        tx->addSignature(b1);

                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                        }

                        applyCheck(tx, *app);

                        REQUIRE(tx->getResult().feeCharged ==
                                2 * app->getLedgerManager().getLastTxFee());
                        REQUIRE(tx->getResultCode() == txFAILED);
                        // first operation was success
                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    getFirstResult(*tx)) == PAYMENT_SUCCESS);
                        // second
                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    tx->getOperations()[1]->getResult()) ==
                                PAYMENT_UNDERFUNDED);
                    }
                    SECTION("both success")
                    {
                        auto tx = a1.tx(
                            {payment(root, 1000), b1.op(payment(root, 1000))});
                        getSignatures(tx).clear();
                        tx->addSignature(a1);
                        tx->addSignature(b1);

                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                        }

                        applyCheck(tx, *app);

                        REQUIRE(tx->getResult().feeCharged ==
                                2 * app->getLedgerManager().getLastTxFee());
                        REQUIRE(tx->getResultCode() == txSUCCESS);

                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    getFirstResult(*tx)) == PAYMENT_SUCCESS);
                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    tx->getOperations()[1]->getResult()) ==
                                PAYMENT_SUCCESS);
                    }
                });
            }
            SECTION("operation using default signature")
            {
                for_all_versions(*app, [&] {
                    auto c1 = TestAccount{*app, getAccount("C")};

                    // build a transaction:
                    //  1. B funds C
                    //  2. send from C -> root

                    auto tx = b1.tx(
                        {createAccount(c1.getPublicKey(), paymentAmount / 2),
                         c1.op(payment(root, 1000))});

                    tx->addSignature(c1);

                    applyCheck(tx, *app);

                    REQUIRE(tx->getResult().feeCharged ==
                            2 * app->getLedgerManager().getLastTxFee());
                    REQUIRE(tx->getResultCode() == txSUCCESS);

                    REQUIRE(CreateAccountOpFrame::getInnerCode(
                                getFirstResult(*tx)) == CREATE_ACCOUNT_SUCCESS);
                    REQUIRE(PaymentOpFrame::getInnerCode(
                                tx->getOperations()[1]->getResult()) ==
                            PAYMENT_SUCCESS);
                });
            }
        }
    }

    SECTION("common transaction")
    {
        auto a1 = root.create("A", paymentAmount);

        TransactionFramePtr txFrame;
        auto setup = [&]() {
            auto txSet = std::make_shared<TxSetFrame>(
                app->getLedgerManager().getLastClosedLedgerHeader().hash);

            txFrame = root.tx({createAccount(a1, paymentAmount)});
            txSet->add(txFrame);

            // close this ledger
            StellarValue sv = app->getHerder().makeStellarValue(
                txSet->getContentsHash(), 1, emptyUpgradeSteps,
                app->getConfig().NODE_SEED);
            LedgerCloseData ledgerData(1, txSet, sv);
            app->getLedgerManager().closeLedger(ledgerData);

            REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() == 2);
        };

        {
            SECTION("Insufficient fee")
            {
                for_all_versions(*app, [&] {
                    setup();
                    txFrame =
                        root.tx({payment(a1.getPublicKey(), paymentAmount)});
                    setFee(txFrame, app->getLedgerManager().getLastTxFee() - 1);

                    applyCheck(txFrame, *app);

                    REQUIRE(txFrame->getResultCode() == txINSUFFICIENT_FEE);
                    // during apply, feeCharged is smaller in this case
                    REQUIRE(txFrame->getResult().feeCharged ==
                            app->getLedgerManager().getLastTxFee() - 1);
                });
            }

            SECTION("duplicate payment")
            {
                for_versions_to(9, *app, [&] {
                    setup();
                    {
                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        REQUIRE(!txFrame->checkValid(ltx, 0, 0, 0));
                    }
                    REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
                });
                for_versions_from(10, *app, [&] {
                    setup();
                    applyCheck(txFrame, *app);

                    REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
                });
            }

            SECTION("time issues")
            {
                for_all_versions(*app, [&] {
                    setup();
                    // tx too young
                    // tx ok
                    // tx too old
                    VirtualClock::system_time_point ledgerTime;
                    time_t start = getTestDate(1, 7, 2014);
                    ledgerTime = VirtualClock::from_time_t(start);

                    clock.setCurrentVirtualTime(ledgerTime);

                    SECTION("too early")
                    {
                        txFrame = root.tx(
                            {payment(a1.getPublicKey(), paymentAmount)});
                        setMinTime(txFrame, start + 1000);
                        setMaxTime(txFrame, start + 10000);

                        closeLedgerOn(*app, 3, start + 1);
                        applyCheck(txFrame, *app);

                        REQUIRE(txFrame->getResultCode() == txTOO_EARLY);
                    }

                    SECTION("on time")
                    {
                        txFrame = root.tx(
                            {payment(a1.getPublicKey(), paymentAmount)});
                        setMinTime(txFrame, 1000);
                        setMaxTime(txFrame, start + 300000);
                        getSignatures(txFrame).clear();
                        txFrame->addSignature(root);

                        closeLedgerOn(*app, 3, start + 1);
                        applyCheck(txFrame, *app);
                        REQUIRE(txFrame->getResultCode() == txSUCCESS);
                    }

                    SECTION("too late")
                    {
                        txFrame = root.tx(
                            {payment(a1.getPublicKey(), paymentAmount)});
                        setMinTime(txFrame, 1000);
                        setMaxTime(txFrame, start);

                        closeLedgerOn(*app, 3, start + 1);
                        applyCheck(txFrame, *app);
                        REQUIRE(txFrame->getResultCode() == txTOO_LATE);
                    }

                    SECTION("lower bound offset")
                    {
                        txFrame = root.tx(
                            {payment(a1.getPublicKey(), paymentAmount)});

                        setMaxTime(txFrame, 0);

                        TimePoint lastClose = getTestDate(1, 1, 2020);
                        closeLedgerOn(*app, 3, lastClose);

                        TimePoint const nextOffset = 2;
                        auto const nextClose = lastClose + nextOffset;

                        auto testOneMinTimeLowerBoundCombination =
                            [&](TimePoint const minTime,
                                TimePoint const lowerBound,
                                bool const expectSuccess) {
                                setMinTime(txFrame, minTime);
                                getSignatures(txFrame).clear();
                                txFrame->addSignature(root);
                                {
                                    LedgerTxn ltx(app->getLedgerTxnRoot());
                                    REQUIRE(txFrame->checkValid(
                                                ltx, 0, lowerBound, 0) ==
                                            expectSuccess);
                                }
                                REQUIRE(
                                    txFrame->getResultCode() ==
                                    (expectSuccess ? txSUCCESS : txTOO_EARLY));
                            };

                        SECTION("too early despite offset")
                        {
                            testOneMinTimeLowerBoundCombination(
                                nextClose + 1, nextOffset, false);
                        }

                        SECTION("definitely too early without offset")
                        {
                            testOneMinTimeLowerBoundCombination(nextClose + 1,
                                                                0, false);
                        }

                        SECTION("not too early because of offset")
                        {
                            testOneMinTimeLowerBoundCombination(
                                nextClose, nextOffset, true);
                        }

                        SECTION("would have been too early without offset")
                        {
                            testOneMinTimeLowerBoundCombination(nextClose, 0,
                                                                false);
                        }

                        SECTION("not too early even without offset")
                        {
                            testOneMinTimeLowerBoundCombination(lastClose, 0,
                                                                true);
                        }

                        SECTION("definitely not too early with offset")
                        {
                            testOneMinTimeLowerBoundCombination(
                                lastClose, nextOffset, true);
                        }
                    }

                    SECTION("upper bound offset")
                    {
                        txFrame = root.tx(
                            {payment(a1.getPublicKey(), paymentAmount)});

                        setMinTime(txFrame, 1000);
                        getSignatures(txFrame).clear();
                        txFrame->addSignature(root);

                        closeLedgerOn(*app, 3, 3, 7, 2014);

                        auto closeTime = app->getLedgerManager()
                                             .getLastClosedLedgerHeader()
                                             .header.scpValue.closeTime;

                        auto offsetTest = [&](bool pushTime) {
                            if (pushTime)
                            {
                                // move clock past close time
                                clock.setCurrentVirtualTime(
                                    VirtualClock::from_time_t(closeTime + 5));
                            }

                            auto offset =
                                getUpperBoundCloseTimeOffset(*app, closeTime);
                            auto upperBoundCloseTime = closeTime + offset;

                            SECTION("success")
                            {
                                setMaxTime(txFrame, upperBoundCloseTime);
                                getSignatures(txFrame).clear();
                                txFrame->addSignature(root);

                                {
                                    LedgerTxn ltx(app->getLedgerTxnRoot());
                                    REQUIRE(
                                        txFrame->checkValid(ltx, 0, 0, offset));
                                }

                                REQUIRE(txFrame->getResultCode() == txSUCCESS);
                            }

                            SECTION("too late")
                            {
                                setMaxTime(txFrame, upperBoundCloseTime - 1);

                                {
                                    LedgerTxn ltx(app->getLedgerTxnRoot());
                                    REQUIRE(!txFrame->checkValid(ltx, 0, 0,
                                                                 offset));
                                }

                                REQUIRE(txFrame->getResultCode() == txTOO_LATE);
                            }
                        };

                        SECTION("current time behind closeTime")
                        {
                            offsetTest(true);
                        }
                        SECTION("current time past closeTime")
                        {
                            offsetTest(false);
                        }
                    }
                });
            }

            SECTION("transaction gap")
            {
                for_versions_to(9, *app, [&] {
                    setup();
                    txFrame =
                        root.tx({payment(a1.getPublicKey(), paymentAmount)});
                    setSeqNum(txFrame, txFrame->getSeqNum() - 1);
                    {
                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        REQUIRE(!txFrame->checkValid(ltx, 0, 0, 0));
                    }

                    REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
                });
                for_versions_from(10, *app, [&] {
                    setup();
                    txFrame =
                        root.tx({payment(a1.getPublicKey(), paymentAmount)});
                    setSeqNum(txFrame, txFrame->getSeqNum() - 1);
                    applyCheck(txFrame, *app);

                    REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
                });
            }
        }
    }

    SECTION("change signer and weights mid-transaction")
    {
        auto a = root.create("a", paymentAmount);
        auto b = root.create("b", paymentAmount);
        auto const baseFee = app->getLedgerManager().getLastTxFee();

        SECTION("switch a into regular account 1")
        {
            a.setOptions(setSigner(makeSigner(b, 1)) | setMasterWeight(1) |
                         setLowThreshold(2) | setMedThreshold(2) |
                         setHighThreshold(2));

            for_all_versions(*app, [&] {
                auto tx = a.tx({setOptions(setMasterWeight(2) |
                                           setSigner(makeSigner(b, 0)))});
                tx->addSignature(b);

                validateTxResults(tx, *app, {baseFee * 1, txSUCCESS},
                                  expectedResult(baseFee * 1, 1, txSUCCESS,
                                                 {SET_OPTIONS_SUCCESS}));
            });
        }

        SECTION("switch a into regular account 2")
        {
            a.setOptions(setMasterWeight(1) | setLowThreshold(2) |
                         setMedThreshold(2) | setHighThreshold(2) |
                         setSigner(makeSigner(b, 1)));

            for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                auto tx = a.tx({setOptions(setSigner(makeSigner(b, 0))),
                                setOptions(setMasterWeight(2))});
                tx->addSignature(b);

                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txFAILED,
                                   {SET_OPTIONS_SUCCESS, opBAD_AUTH}));
            });
            for_versions_from({7, 10}, *app, [&] {
                auto tx = a.tx({setOptions(setSigner(makeSigner(b, 0))),
                                setOptions(setMasterWeight(2))});
                tx->addSignature(b);

                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txSUCCESS,
                                   {SET_OPTIONS_SUCCESS, SET_OPTIONS_SUCCESS}));
            });
        }

        SECTION("merge one of signing accounts")
        {
            a.setOptions(setMasterWeight(0) | setSigner(makeSigner(b, 1)));
            closeLedgerOn(*app, 2, 1, 1, 2016);

            SECTION("by destination")
            {
                for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                    auto tx =
                        b.tx({a.op(setOptions(setMasterWeight(1) |
                                              setSigner(makeSigner(b, 0)))),
                              a.op(accountMerge(b))});

                    validateTxResults(
                        tx, *app, {baseFee * 2, txSUCCESS},
                        expectedResult(baseFee * 2, 2, txFAILED,
                                       {SET_OPTIONS_SUCCESS, opBAD_AUTH}));
                });
                for_versions_from({7, 10}, *app, [&] {
                    auto tx =
                        b.tx({a.op(setOptions(setMasterWeight(1) |
                                              setSigner(makeSigner(b, 0)))),
                              a.op(accountMerge(b))});

                    auto applyResult = expectedResult(
                        baseFee * 2, 2, txSUCCESS,
                        {SET_OPTIONS_SUCCESS,
                         {ACCOUNT_MERGE_SUCCESS, paymentAmount - 100}});
                    validateTxResults(tx, *app, {baseFee * 2, txSUCCESS},
                                      applyResult);
                });
            }

            SECTION("by source, signed by destination")
            {
                for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                    auto tx = a.tx({setOptions(setMasterWeight(1) |
                                               setSigner(makeSigner(b, 0))),
                                    accountMerge(b)});
                    getSignatures(tx).clear();
                    tx->addSignature(b);

                    validateTxResults(
                        tx, *app, {baseFee * 2, txSUCCESS},
                        expectedResult(baseFee * 2, 2, txFAILED,
                                       {SET_OPTIONS_SUCCESS, opBAD_AUTH}));
                });
                for_versions_from({7, 10}, *app, [&] {
                    auto tx = a.tx({setOptions(setMasterWeight(1) |
                                               setSigner(makeSigner(b, 0))),
                                    accountMerge(b)});
                    getSignatures(tx).clear();
                    tx->addSignature(b);

                    auto applyResult = expectedResult(
                        baseFee * 2, 2, txSUCCESS,
                        {SET_OPTIONS_SUCCESS,
                         {ACCOUNT_MERGE_SUCCESS, paymentAmount - 300}});
                    validateTxResults(tx, *app, {baseFee * 2, txSUCCESS},
                                      applyResult);
                });
            }

            SECTION("by source, signed by both")
            {
                for_all_versions_except({7}, *app, [&] {
                    auto tx = a.tx({setOptions(setMasterWeight(1) |
                                               setSigner(makeSigner(b, 0))),
                                    accountMerge(b)});
                    tx->addSignature(b);

                    validateTxResults(tx, *app,
                                      {baseFee * 2, txBAD_AUTH_EXTRA});
                });
                for_versions({7}, *app, [&] {
                    auto tx = a.tx({setOptions(setMasterWeight(1) |
                                               setSigner(makeSigner(b, 0))),
                                    accountMerge(b)});
                    tx->addSignature(b);

                    auto applyResult = expectedResult(
                        baseFee * 2, 2, txSUCCESS,
                        {SET_OPTIONS_SUCCESS,
                         {ACCOUNT_MERGE_SUCCESS, paymentAmount - 300}});
                    validateTxResults(tx, *app, {baseFee * 2, txSUCCESS},
                                      applyResult);
                });
            }
        }

        SECTION("change thresholds twice")
        {
            for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                auto tx = a.tx({setOptions(setHighThreshold(3)),
                                setOptions(setHighThreshold(3))});

                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txFAILED,
                                   {SET_OPTIONS_SUCCESS, opBAD_AUTH}));
            });
            for_versions_from({7, 10}, *app, [&] {
                auto tx = a.tx({setOptions(setHighThreshold(3)),
                                setOptions(setHighThreshold(3))});

                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txSUCCESS,
                                   {SET_OPTIONS_SUCCESS, SET_OPTIONS_SUCCESS}));
            });
        }

        SECTION("lower master weight twice")
        {
            a.setOptions(setMasterWeight(10) | setLowThreshold(1) |
                         setMedThreshold(5) | setHighThreshold(10));

            for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                auto tx = a.tx({setOptions(setMasterWeight(9)),
                                setOptions(setMasterWeight(8))});

                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txFAILED,
                                   {SET_OPTIONS_SUCCESS, opBAD_AUTH}));
            });
            for_versions_from({7, 10}, *app, [&] {
                auto tx = a.tx({setOptions(setMasterWeight(9)),
                                setOptions(setMasterWeight(8))});

                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txSUCCESS,
                                   {SET_OPTIONS_SUCCESS, SET_OPTIONS_SUCCESS}));
            });
        }

        SECTION("reduce auth, do something")
        {
            a.setOptions(setSigner(makeSigner(b, 2)) | setMasterWeight(3) |
                         setLowThreshold(1) | setMedThreshold(3) |
                         setHighThreshold(5));

            SECTION("single tx")
            {
                SECTION("valid")
                {
                    for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                        auto tx =
                            a.tx({setOptions(setSigner(makeSigner(b, 1))),
                                  setOptions(setSigner(makeSigner(b, 2)))});
                        tx->addSignature(b);

                        validateTxResults(
                            tx, *app, {baseFee * 2, txSUCCESS},
                            expectedResult(baseFee * 2, 2, txFAILED,
                                           {SET_OPTIONS_SUCCESS, opBAD_AUTH}));
                    });
                    for_versions_from({7, 10}, *app, [&] {
                        auto tx =
                            a.tx({setOptions(setSigner(makeSigner(b, 1))),
                                  setOptions(setSigner(makeSigner(b, 2)))});
                        tx->addSignature(b);

                        validateTxResults(
                            tx, *app, {baseFee * 2, txSUCCESS},
                            expectedResult(
                                baseFee * 2, 2, txSUCCESS,
                                {SET_OPTIONS_SUCCESS, SET_OPTIONS_SUCCESS}));
                    });
                }
                SECTION("missing signature")
                {
                    for_versions_from(10, *app, [&] {
                        auto tx =
                            a.tx({payment(root, 1000),
                                  setOptions(setSigner(makeSigner(b, 2)))});
                        // missing b signature
                        applyCheck(tx, *app);
                        REQUIRE(tx->getResultCode() == txFAILED);
                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    getFirstResult(*tx)) == PAYMENT_SUCCESS);
                        REQUIRE(tx->getOperations()[1]->getResultCode() ==
                                opBAD_AUTH);
                    });
                }
            }
            SECTION("multiple tx")
            {
                for_versions_from(10, *app, [&] {
                    auto tx1 = a.tx({setOptions(setSigner(makeSigner(b, 1)))});
                    tx1->addSignature(b);
                    auto tx2 = a.tx({payment(root, 1000),
                                     setOptions(setSigner(makeSigner(b, 2)))});
                    tx2->addSignature(b);

                    auto r = closeLedgerOn(*app, 2, 1, 2, 2016, {tx1, tx2});

                    REQUIRE(tx1->getResultCode() == txSUCCESS);
                    REQUIRE(tx2->getResultCode() == txFAILED);
                    REQUIRE(PaymentOpFrame::getInnerCode(
                                getFirstResult(*tx2)) == PAYMENT_SUCCESS);
                    REQUIRE(tx2->getOperations()[1]->getResultCode() ==
                            opBAD_AUTH);
                });
            }
        }

        SECTION("add signer, increase thresholds, do something")
        {
            for_all_versions_except({7}, *app, [&] {
                auto tx =
                    a.tx({setOptions(setSigner(makeSigner(b, 1)) |
                                     setMasterWeight(1) | setLowThreshold(2) |
                                     setMedThreshold(2) | setHighThreshold(2)),
                          setOptions(setHomeDomain("stellar.org"))});
                tx->addSignature(b);

                validateTxResults(tx, *app, {baseFee * 2, txBAD_AUTH_EXTRA});
            });
            for_versions({7}, *app, [&] {
                auto tx =
                    a.tx({setOptions(setSigner(makeSigner(b, 1)) |
                                     setMasterWeight(1) | setLowThreshold(2) |
                                     setMedThreshold(2) | setHighThreshold(2)),
                          setOptions(setHomeDomain("stellar.org"))});
                tx->addSignature(b);

                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txSUCCESS,
                                   {SET_OPTIONS_SUCCESS, SET_OPTIONS_SUCCESS}));
            });
        }
    }

    SECTION("mux accounts")
    {
        auto toMux = [](MuxedAccount& id, uint64 memoID) {
            MuxedAccount muxedID(KEY_TYPE_MUXED_ED25519);
            auto& mid = muxedID.med25519();
            mid.ed25519 = id.ed25519();
            mid.id = memoID;
            id = muxedID;
        };
        auto a = root.create("A", paymentAmount);

        auto doChecks = [&](TransactionResultCode res) {
            Operation op = txtest::payment(a.getPublicKey(), paymentAmount);
            auto& pay = op.body.paymentOp();

            auto checkRes = [&]() {
                auto txFrame = root.tx({op});
                applyCheck(txFrame, *app, false);
                REQUIRE(txFrame->getResultCode() == res);
            };

            SECTION("dest account")
            {
                toMux(pay.destination, 2);
                checkRes();
            }
            SECTION("src account")
            {
                op = root.op(op);
                toMux(*op.sourceAccount, 3);
                checkRes();
            }
        };

        for_versions({12}, *app, [&] { doChecks(txNOT_SUPPORTED); });
        for_versions_from(13, *app, [&] { doChecks(txSUCCESS); });
    }
}
