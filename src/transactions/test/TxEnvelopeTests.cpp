// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SignerKey.h"
#include "crypto/SignerKeyUtils.h"
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
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/Timer.h"

using namespace stellar;
using namespace stellar::txtest;

/*
  Tests that are testing the common envelope used in transactions.
  Things like:
    authz/authn
    double spend
*/

TEST_CASE("txenvelope", "[tx][envelope]")
{
    Config cfg = getTestConfig();

    // Do our setup in version 1 so that for_all_versions below does not
    // try to downgrade us from >1 to 1.
    cfg.LEDGER_PROTOCOL_VERSION = 1;

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
            auto txFrame =
                root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
            txFrame->getEnvelope().signatures.clear();

            for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txSUCCESS);
            });
        }

        SECTION("bad signature")
        {
            auto txFrame =
                root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
            txFrame->getEnvelope().signatures[0].signature = Signature(32, 123);

            for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txSUCCESS);
            });
        }

        SECTION("bad signature (wrong hint)")
        {
            auto txFrame =
                root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
            txFrame->getEnvelope().signatures[0].hint.fill(1);

            for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txSUCCESS);
            });
        }

        SECTION("too many signatures (signed twice)")
        {
            auto txFrame =
                root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
            txFrame->addSignature(a1);

            for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txBAD_AUTH_EXTRA);
            });
            for_versions({7}, *app, [&] {
                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txSUCCESS);
            });
        }

        SECTION("too many signatures (unused signature)")
        {
            auto txFrame =
                root.tx({createAccount(a1.getPublicKey(), paymentAmount)});
            SecretKey bogus = getAccount("bogus");
            txFrame->addSignature(bogus);

            for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                applyCheck(txFrame, *app);
                REQUIRE(txFrame->getResultCode() == txBAD_AUTH_EXTRA);
            });
            for_versions({7}, *app, [&] {
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
            auto tx = a1.tx({payment(root, 1000)});

            // only sign with s1
            tx->getEnvelope().signatures.clear();
            tx->addSignature(s1);

            for_all_versions_except({7}, *app, [&] {
                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
            });
        }

        SECTION("not enough rights (operation, together)")
        {
            // updating thresholds requires high
            auto tx = a1.tx({setOptions(th | setSigner(sk1))});

            // only sign with s2 (med)
            tx->getEnvelope().signatures.clear();
            tx->addSignature(s2);

            for_all_versions_except({7}, *app, [&] {
                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txFAILED);
                REQUIRE(getFirstResultCode(*tx) == opBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
            });
        }

        SECTION("not enough rights (first thresholds)")
        {
            // updating thresholds requires high
            auto tx = a1.tx({setOptions(th), setOptions(setSigner(sk1))});

            // only sign with s2 (med)
            tx->getEnvelope().signatures.clear();
            tx->addSignature(s2);

            for_all_versions_except({7}, *app, [&] {
                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txFAILED);
                REQUIRE(getFirstResultCode(*tx) == opBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
            });
        }

        SECTION("not enough rights (first signer)")
        {
            // updating thresholds requires high
            auto tx = a1.tx({setOptions(setSigner(sk1)), setOptions(th)});

            // only sign with s2 (med)
            tx->getEnvelope().signatures.clear();
            tx->addSignature(s2);

            for_all_versions_except({7}, *app, [&] {
                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txFAILED);
                REQUIRE(getFirstResultCode(*tx) == opBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
            });
        }

        SECTION("success two signatures, together")
        {
            // updating thresholds requires high
            auto tx = a1.tx({setOptions(th | setSigner(sk1))});

            tx->getEnvelope().signatures.clear();
            tx->addSignature(s1);
            tx->addSignature(s2);

            for_all_versions(*app, [&] {
                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
                REQUIRE(SetOptionsOpFrame::getInnerCode(getFirstResult(*tx)) ==
                        SET_OPTIONS_SUCCESS);
            });
        }

        SECTION("success two signatures, first thresholds")
        {
            // updating thresholds requires high
            auto tx = a1.tx({setOptions(th), setOptions(setSigner(sk1))});

            tx->getEnvelope().signatures.clear();
            tx->addSignature(s1);
            tx->addSignature(s2);

            for_all_versions(*app, [&] {
                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txSUCCESS);
                REQUIRE(SetOptionsOpFrame::getInnerCode(getFirstResult(*tx)) ==
                        SET_OPTIONS_SUCCESS);
            });
        }

        SECTION("success two signatures, first signer")
        {
            // updating thresholds requires high
            auto tx = a1.tx({setOptions(setSigner(sk1)), setOptions(th)});

            tx->getEnvelope().signatures.clear();
            tx->addSignature(s1);
            tx->addSignature(s2);

            for_all_versions(*app, [&] {
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
                // payment requires medium
                auto tx = a1.tx({payment(root, 1000)});
                // only sign with s2 (med)
                if (!withMaster)
                {
                    tx->getEnvelope().signatures.clear();
                }
                tx->addSignature(s2);

                for_all_versions_except({7}, *app, [&] {
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
            auto tx = a1.tx({payment(root, 1000)});

            tx->getEnvelope().signatures.clear();
            for (auto i = 0; i < 10; i++)
                tx->addSignature(s1);

            for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                applyCheck(tx, *app);
                REQUIRE(tx->getResultCode() == txBAD_AUTH);
            });
            for_versions({7}, *app, [&] {
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
            std::function<SignerKey(TransactionFrame const&)> createSigner;
            std::function<void(TransactionFrame&)> sign;
        };

        // ensue that hash(x) supports 0 inside 'x'
        auto x = std::vector<uint8_t>{'a', 'b', 'c', 0,   'd', 'e', 'f', 0,
                                      0,   0,   'g', 'h', 'i', 'j', 'k', 'l',
                                      'A', 'B', 'C', 0,   'D', 'E', 'F', 0,
                                      0,   0,   'G', 'H', 'I', 'J', 'K', 'L'};
        auto alternatives = std::vector<AltSignature>{
            AltSignature{"hash tx", true,
                         [](TransactionFrame const& tx) {
                             return SignerKeyUtils::preAuthTxKey(tx);
                         },
                         [](TransactionFrame&) {}},
            AltSignature{"hash x", false,
                         [x](TransactionFrame const&) {
                             return SignerKeyUtils::hashXKey(x);
                         },
                         [x](TransactionFrame& tx) {
                             tx.addSignature(SignatureUtils::signHashX(x));
                         }}};

        for (auto const& alternative : alternatives)
        {
            SECTION(alternative.name)
            {
                for_versions_to(2, *app, [&] {
                    auto tx = a1.tx({payment(root, 1000)});
                    tx->getEnvelope().signatures.clear();
                    tx->getEnvelope().tx.seqNum++;
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
                        auto tx = a1.tx({payment(root, 1000)});
                        tx->getEnvelope().signatures.clear();
                        a1.setSequenceNumber(a1.getLastSequenceNumber() - 1);
                        auto setup = [&]() {
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
                                REQUIRE(!tx->checkValid(ltx, 0));
                            }
                            REQUIRE(tx->getResultCode() == txBAD_SEQ);
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                        });
                        for_versions_from(10, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_SEQ);
                            REQUIRE(getAccountSigners(a1, *app).size() == 1);
                        });
                    }

                    SECTION("invalid signature")
                    {
                        auto tx = a1.tx({payment(root, 1000)});
                        tx->getEnvelope().signatures.clear();
                        tx->getEnvelope().tx.seqNum++;
                        a1.setSequenceNumber(a1.getLastSequenceNumber() - 1);
                        auto setup = [&]() {
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
                        auto tx = a1.tx({payment(root, 1000)});
                        tx->getEnvelope().tx.seqNum++;
                        a1.setSequenceNumber(a1.getLastSequenceNumber() - 1);
                        auto setup = [&]() {
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
                        auto tx = a1.tx({payment(root, 1000)});
                        tx->getEnvelope().signatures.clear();
                        tx->getEnvelope().tx.seqNum++;
                        a1.setSequenceNumber(a1.getLastSequenceNumber() - 1);
                        auto setup = [&]() {
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

                    SECTION("accountMerge signing account")
                    {
                        auto b1 = root.create("b1", paymentAmount);
                        a1.pay(b1, 1000);

                        closeLedgerOn(*app, 2, 1, 1, 2016);

                        auto tx = b1.tx({accountMerge(a1)},
                                        b1.getLastSequenceNumber() + 2);
                        tx->getEnvelope().signatures.clear();

                        for_versions_from(3, *app, [&] {
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
                        auto tx = a1.tx({payment(root, -1)});
                        tx->getEnvelope().signatures.clear();
                        tx->getEnvelope().tx.seqNum++;
                        a1.setSequenceNumber(a1.getLastSequenceNumber() - 1);
                        auto setup = [&]() {
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
                        auto tx = a1.tx({payment(root, 1000)});
                        tx->getEnvelope().signatures.clear();
                        tx->getEnvelope().tx.seqNum++;
                        a1.setSequenceNumber(a1.getLastSequenceNumber() - 1);
                        auto setup = [&]() {
                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 5); // below low rights
                            a1.setOptions(setSigner(sk1));
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                            alternative.sign(*tx);
                        };
                        for_versions_from({3, 4, 5, 6, 8}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH);
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                        });
                        for_versions({7}, *app, [&] {
                            setup();
                            applyCheck(tx, *app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(getAccountSigners(a1, *app).size() == 2);
                        });
                    }

                    SECTION("not enough rights (operation)")
                    {
                        // updating thresholds requires high
                        auto tx = a1.tx({setOptions(th)},
                                        a1.getLastSequenceNumber() + 2);
                        tx->getEnvelope().signatures.clear();
                        auto setup = [&]() {
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
                        auto b = root.create("b", 1000000000);
                        auto tx1 = b.tx({setOptions(
                            setMasterWeight(1) | setLowThreshold(1) |
                            setMedThreshold(2) | setHighThreshold(3))});
                        auto tx2 =
                            b.tx({payment(root, 100), root.op(payment(b, 100))},
                                 b.getLastSequenceNumber() + 1);
                        auto setup = [&]() {
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
                        auto b = root.create("b", 1000000000);
                        auto tx1 = b.tx({setOptions(
                            setMasterWeight(1) | setLowThreshold(1) |
                            setMedThreshold(2) | setHighThreshold(3))});
                        auto tx2 =
                            b.tx({root.op(payment(b, 100)), payment(root, 100)},
                                 b.getLastSequenceNumber() + 1);
                        auto setup = [&]() {
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

                    SECTION("success signature + " + alternative.name)
                    {
                        auto tx = a1.tx({payment(root, 1000)});
                        tx->getEnvelope().signatures.clear();
                        tx->addSignature(s1);
                        tx->getEnvelope().tx.seqNum++;
                        a1.setSequenceNumber(a1.getLastSequenceNumber() - 1);
                        auto setup = [&]() {
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
                    auto op = a1.op(payment(root, 100));
                    auto tx = transactionFromOperations(
                        *app, root, root.getLastSequenceNumber() + 2, {op});
                    tx->getEnvelope().signatures.clear();
                    auto setup = [&]() {
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
                    auto op = a1.op(payment(root, 100));
                    TransactionFramePtr tx = transactionFromOperations(
                        *app, root, root.getLastSequenceNumber() + 2, {op, op});
                    tx->getEnvelope().signatures.clear();
                    auto setup = [&]() {
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

            auto tx = a1.tx({payment(root, 1000)});
            tx->getEnvelope().signatures.clear();
            tx->addSignature(s1);
            tx->getEnvelope().tx.seqNum++;
            a1.setSequenceNumber(a1.getLastSequenceNumber() - 1);

            for_versions_from(3, *app, [&] {
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
                TransactionEnvelope te;
                te.tx.sourceAccount = root.getPublicKey();
                te.tx.fee = 1000;
                te.tx.seqNum = root.nextSequenceNumber();
                TransactionFramePtr tx =
                    std::make_shared<TransactionFrame>(app->getNetworkID(), te);
                tx->addSignature(root);

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(!tx->checkValid(ltx, 0));
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
                auto tx = a1.tx({payment(root, 1000)});

                // change inner payment to be b->root
                tx->getEnvelope().tx.operations[0].sourceAccount.activate() =
                    b1.getPublicKey();

                tx->getEnvelope().signatures.clear();
                tx->addSignature(a1);

                SECTION("missing signature")
                {
                    for_versions_from({1, 2, 3, 4, 5, 6, 8}, *app, [&] {
                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(!tx->checkValid(ltx, 0));
                        }
                        applyCheck(tx, *app);
                        REQUIRE(tx->getResultCode() == txFAILED);
                        REQUIRE(tx->getOperations()[0]->getResultCode() ==
                                opBAD_AUTH);
                    });
                    for_versions({7}, *app, [&] {
                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(tx->checkValid(ltx, 0));
                        }
                        applyCheck(tx, *app);
                        REQUIRE(tx->getResultCode() == txSUCCESS);
                    });
                }

                SECTION("success")
                {
                    for_all_versions(*app, [&] {
                        tx->addSignature(b1);

                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(tx->checkValid(ltx, 0));
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
                    auto tx_a = a1.tx({payment(root, 1000)});
                    SECTION("one invalid tx")
                    {
                        auto idr = b1.asset("IDR");
                        Price price(1, 1);
                        auto tx_b =
                            b1.tx({manageOffer(0, idr, idr, price, 1000)});

                        // build a new tx based off tx_a and tx_b
                        tx_b->getEnvelope()
                            .tx.operations[0]
                            .sourceAccount.activate() = b1.getPublicKey();
                        tx_a->getEnvelope().tx.operations.push_back(
                            tx_b->getEnvelope().tx.operations[0]);
                        tx_a->getEnvelope().tx.fee *= 2;
                        TransactionFramePtr tx =
                            TransactionFrame::makeTransactionFromWire(
                                app->getNetworkID(), tx_a->getEnvelope());

                        tx->getEnvelope().signatures.clear();
                        tx->addSignature(a1);
                        tx->addSignature(b1);

                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(!tx->checkValid(ltx, 0));
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
                        auto tx_b = b1.tx({payment(root, paymentAmount)});

                        tx_b->getEnvelope()
                            .tx.operations[0]
                            .sourceAccount.activate() = b1.getPublicKey();
                        tx_a->getEnvelope().tx.operations.push_back(
                            tx_b->getEnvelope().tx.operations[0]);
                        tx_a->getEnvelope().tx.fee *= 2;
                        TransactionFramePtr tx =
                            TransactionFrame::makeTransactionFromWire(
                                app->getNetworkID(), tx_a->getEnvelope());

                        tx->getEnvelope().signatures.clear();
                        tx->addSignature(a1);
                        tx->addSignature(b1);

                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(tx->checkValid(ltx, 0));
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
                        auto tx_b = b1.tx({payment(root, 1000)});

                        tx_b->getEnvelope()
                            .tx.operations[0]
                            .sourceAccount.activate() = b1.getPublicKey();
                        tx_a->getEnvelope().tx.operations.push_back(
                            tx_b->getEnvelope().tx.operations[0]);
                        tx_a->getEnvelope().tx.fee *= 2;
                        TransactionFramePtr tx =
                            TransactionFrame::makeTransactionFromWire(
                                app->getNetworkID(), tx_a->getEnvelope());

                        tx->getEnvelope().signatures.clear();
                        tx->addSignature(a1);
                        tx->addSignature(b1);

                        {
                            LedgerTxn ltx(app->getLedgerTxnRoot());
                            REQUIRE(tx->checkValid(ltx, 0));
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

        TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
            app->getLedgerManager().getLastClosedLedgerHeader().hash);

        TransactionFramePtr txFrame;

        txFrame = root.tx({createAccount(a1, paymentAmount)});
        txSet->add(txFrame);

        // close this ledger
        StellarValue sv(txSet->getContentsHash(), 1, emptyUpgradeSteps, 0);
        LedgerCloseData ledgerData(1, txSet, sv);
        app->getLedgerManager().closeLedger(ledgerData);

        REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() == 2);

        {
            SECTION("Insufficient fee")
            {
                for_all_versions(*app, [&] {
                    txFrame =
                        root.tx({payment(a1.getPublicKey(), paymentAmount)});
                    txFrame->getEnvelope().tx.fee = static_cast<uint32_t>(
                        app->getLedgerManager().getLastTxFee() - 1);

                    applyCheck(txFrame, *app);

                    REQUIRE(txFrame->getResultCode() == txINSUFFICIENT_FEE);
                });
            }

            SECTION("duplicate payment")
            {
                for_versions_to(9, *app, [&] {
                    {
                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        REQUIRE(!txFrame->checkValid(ltx, 0));
                    }
                    REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
                });
                for_versions_from(10, *app, [&] {
                    applyCheck(txFrame, *app);

                    REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
                });
            }

            SECTION("time issues")
            {
                for_all_versions(*app, [&] {
                    // tx too young
                    // tx ok
                    // tx too old
                    VirtualClock::time_point ledgerTime;
                    time_t start = getTestDate(1, 7, 2014);
                    ledgerTime = VirtualClock::from_time_t(start);

                    clock.setCurrentVirtualTime(ledgerTime);

                    SECTION("too early")
                    {
                        txFrame = root.tx(
                            {payment(a1.getPublicKey(), paymentAmount)});
                        txFrame->getEnvelope().tx.timeBounds.activate() =
                            TimeBounds(start + 1000, start + 10000);

                        closeLedgerOn(*app, 3, 1, 7, 2014);
                        applyCheck(txFrame, *app);

                        REQUIRE(txFrame->getResultCode() == txTOO_EARLY);
                    }

                    SECTION("on time")
                    {
                        txFrame = root.tx(
                            {payment(a1.getPublicKey(), paymentAmount)});
                        txFrame->getEnvelope().tx.timeBounds.activate() =
                            TimeBounds(1000, start + 300000);

                        closeLedgerOn(*app, 3, 2, 7, 2014);
                        applyCheck(txFrame, *app);
                        REQUIRE(txFrame->getResultCode() == txSUCCESS);
                    }

                    SECTION("too late")
                    {
                        txFrame = root.tx(
                            {payment(a1.getPublicKey(), paymentAmount)});
                        txFrame->getEnvelope().tx.timeBounds.activate() =
                            TimeBounds(1000, start);

                        closeLedgerOn(*app, 3, 3, 7, 2014);
                        applyCheck(txFrame, *app);
                        REQUIRE(txFrame->getResultCode() == txTOO_LATE);
                    }
                });
            }

            SECTION("transaction gap")
            {
                for_versions_to(9, *app, [&] {
                    txFrame =
                        root.tx({payment(a1.getPublicKey(), paymentAmount)});
                    txFrame->getEnvelope().tx.seqNum--;
                    {
                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        REQUIRE(!txFrame->checkValid(ltx, 0));
                    }

                    REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
                });
                for_versions_from(10, *app, [&] {
                    txFrame =
                        root.tx({payment(a1.getPublicKey(), paymentAmount)});
                    txFrame->getEnvelope().tx.seqNum--;
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

            auto tx = a.tx(
                {setOptions(setMasterWeight(2) | setSigner(makeSigner(b, 0)))});
            tx->addSignature(b);

            for_all_versions(*app, [&] {
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

            auto tx = a.tx({setOptions(setSigner(makeSigner(b, 0))),
                            setOptions(setMasterWeight(2))});
            tx->addSignature(b);

            for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txFAILED,
                                   {SET_OPTIONS_SUCCESS, opBAD_AUTH}));
            });
            for_versions_from({7, 10}, *app, [&] {
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
                auto tx = b.tx({a.op(setOptions(setMasterWeight(1) |
                                                setSigner(makeSigner(b, 0)))),
                                a.op(accountMerge(b))});

                for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                    validateTxResults(
                        tx, *app, {baseFee * 2, txSUCCESS},
                        expectedResult(baseFee * 2, 2, txFAILED,
                                       {SET_OPTIONS_SUCCESS, opBAD_AUTH}));
                });
                for_versions_from({7, 10}, *app, [&] {
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
                auto tx = a.tx({setOptions(setMasterWeight(1) |
                                           setSigner(makeSigner(b, 0))),
                                accountMerge(b)});
                tx->getEnvelope().signatures.clear();
                tx->addSignature(b);

                for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                    validateTxResults(
                        tx, *app, {baseFee * 2, txSUCCESS},
                        expectedResult(baseFee * 2, 2, txFAILED,
                                       {SET_OPTIONS_SUCCESS, opBAD_AUTH}));
                });
                for_versions_from({7, 10}, *app, [&] {
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
                auto tx = a.tx({setOptions(setMasterWeight(1) |
                                           setSigner(makeSigner(b, 0))),
                                accountMerge(b)});
                tx->addSignature(b);

                for_all_versions_except({7}, *app, [&] {
                    validateTxResults(tx, *app,
                                      {baseFee * 2, txBAD_AUTH_EXTRA});
                });
                for_versions({7}, *app, [&] {
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
            auto tx = a.tx({setOptions(setHighThreshold(3)),
                            setOptions(setHighThreshold(3))});
            for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txFAILED,
                                   {SET_OPTIONS_SUCCESS, opBAD_AUTH}));
            });
            for_versions_from({7, 10}, *app, [&] {
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

            auto tx = a.tx({setOptions(setMasterWeight(9)),
                            setOptions(setMasterWeight(8))});
            for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txFAILED,
                                   {SET_OPTIONS_SUCCESS, opBAD_AUTH}));
            });
            for_versions_from({7, 10}, *app, [&] {
                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txSUCCESS,
                                   {SET_OPTIONS_SUCCESS, SET_OPTIONS_SUCCESS}));
            });
        }

        SECTION("remove signer, do something")
        {
            a.setOptions(setSigner(makeSigner(b, 1)) | setMasterWeight(1) |
                         setLowThreshold(2) | setMedThreshold(2) |
                         setHighThreshold(2));

            auto tx = a.tx({setOptions(setSigner(makeSigner(b, 0))),
                            setOptions(setHomeDomain("stellar.org"))});
            tx->addSignature(b);

            for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txFAILED,
                                   {SET_OPTIONS_SUCCESS, opBAD_AUTH}));
            });
            for_versions_from({7, 10}, *app, [&] {
                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txSUCCESS,
                                   {SET_OPTIONS_SUCCESS, SET_OPTIONS_SUCCESS}));
            });
        }

        SECTION("add signer, increase thresholds, do something")
        {
            auto tx =
                a.tx({setOptions(setSigner(makeSigner(b, 1)) |
                                 setMasterWeight(1) | setLowThreshold(2) |
                                 setMedThreshold(2) | setHighThreshold(2)),
                      setOptions(setHomeDomain("stellar.org"))});
            tx->addSignature(b);

            for_all_versions_except({7}, *app, [&] {
                validateTxResults(tx, *app, {baseFee * 2, txBAD_AUTH_EXTRA});
            });
            for_versions({7}, *app, [&] {
                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txSUCCESS,
                                   {SET_OPTIONS_SUCCESS, SET_OPTIONS_SUCCESS}));
            });
        }
    }
}
