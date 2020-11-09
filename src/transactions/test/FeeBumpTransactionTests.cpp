// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SponsorshipTestUtils.h"

using namespace stellar;
using namespace stellar::txtest;

static void
sign(Hash const& networkID, SecretKey key, TransactionV1Envelope& env)
{
    env.signatures.emplace_back(SignatureUtils::sign(
        key, sha256(xdr::xdr_to_opaque(networkID, ENVELOPE_TYPE_TX, env.tx))));
}

static void
sign(Hash const& networkID, SecretKey key, FeeBumpTransactionEnvelope& env)
{
    env.signatures.emplace_back(SignatureUtils::sign(
        key, sha256(xdr::xdr_to_opaque(networkID, ENVELOPE_TYPE_TX_FEE_BUMP,
                                       env.tx))));
}

static TransactionEnvelope
feeBumpUnsigned(TestAccount& feeSource, TestAccount& source, TestAccount& dest,
                int64_t outerFee, uint32_t innerFee, int64_t amount)
{
    TransactionEnvelope fb(ENVELOPE_TYPE_TX_FEE_BUMP);
    fb.feeBump().tx.feeSource = toMuxedAccount(feeSource);
    fb.feeBump().tx.fee = outerFee;

    auto& env = fb.feeBump().tx.innerTx;
    env.type(ENVELOPE_TYPE_TX);
    env.v1().tx.sourceAccount = toMuxedAccount(source);
    env.v1().tx.fee = innerFee;
    env.v1().tx.seqNum = source.nextSequenceNumber();
    env.v1().tx.operations = {payment(dest, amount)};

    return fb;
}

static TransactionFrameBasePtr
feeBump(Hash const& networkID, TestAccount& feeSource, TestAccount& source,
        TestAccount& dest, int64_t outerFee, uint32_t innerFee, int64_t amount)
{
    auto fb =
        feeBumpUnsigned(feeSource, source, dest, outerFee, innerFee, amount);
    auto& env = fb.feeBump().tx.innerTx;
    sign(networkID, source, env.v1());
    sign(networkID, feeSource, fb.feeBump());
    return TransactionFrameBase::makeTransactionFromWire(networkID, fb);
}

TEST_CASE("fee bump transactions", "[tx][feebump]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    auto& lm = app->getLedgerManager();
    auto fee = lm.getLastClosedLedgerHeader().header.baseFee;
    auto reserve = lm.getLastClosedLedgerHeader().header.baseReserve;

    auto root = TestAccount::createRoot(*app);

    SECTION("validity")
    {
        SECTION("not supported")
        {
            for_versions({12}, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), root, root, root,
                                  2 * fee, fee, 1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(!fb->checkValid(ltx, 0, 0, 0));
                REQUIRE(fb->getResultCode() == txNOT_SUPPORTED);
            });
        }

        SECTION("insufficient fee, less than min")
        {
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), root, root, root,
                                  2 * fee - 1, 1, 1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(!fb->checkValid(ltx, 0, 0, 0));
                REQUIRE(fb->getResultCode() == txINSUFFICIENT_FEE);
                REQUIRE(fb->getResult().feeCharged == 2 * fee);
            });
        }

        SECTION("insufficient fee, rate less than inner")
        {
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), root, root, root,
                                  2 * fee + 1, 101, 1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(!fb->checkValid(ltx, 0, 0, 0));
                REQUIRE(fb->getResultCode() == txINSUFFICIENT_FEE);
                REQUIRE(fb->getResult().feeCharged == 2 * 101);
            });
        }

        SECTION("fee source does not exist")
        {
            auto acc = TestAccount(*app, getAccount("A"));
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, root, root, 2 * fee,
                                  fee, 1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(!fb->checkValid(ltx, 0, 0, 0));
                REQUIRE(fb->getResultCode() == txNO_ACCOUNT);
            });
        }

        SECTION("bad signatures, signature missing")
        {
            auto acc = root.create("A", 2 * reserve);
            for_versions_from(13, *app, [&] {
                auto fbXDR = feeBumpUnsigned(acc, root, root, 2 * fee, fee, 1);
                sign(app->getNetworkID(), root,
                     fbXDR.feeBump().tx.innerTx.v1());
                auto fb = TransactionFrameBase::makeTransactionFromWire(
                    app->getNetworkID(), fbXDR);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(!fb->checkValid(ltx, 0, 0, 0));
                REQUIRE(fb->getResultCode() == txBAD_AUTH);
            });
        }

        SECTION("bad signatures, signature invalid")
        {
            auto acc = root.create("A", 2 * reserve);
            for_versions_from(13, *app, [&] {
                auto fbXDR = feeBumpUnsigned(acc, root, root, 2 * fee, fee, 1);
                // These signatures are applied in the wrong order, so the outer
                // signature is invalid
                sign(app->getNetworkID(), acc, fbXDR.feeBump());
                sign(app->getNetworkID(), root,
                     fbXDR.feeBump().tx.innerTx.v1());
                auto fb = TransactionFrameBase::makeTransactionFromWire(
                    app->getNetworkID(), fbXDR);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(!fb->checkValid(ltx, 0, 0, 0));
                REQUIRE(fb->getResultCode() == txBAD_AUTH);
            });
        }

        SECTION("insufficient balance")
        {
            auto acc = root.create("A", 2 * reserve);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, root, root, 2 * fee,
                                  fee, 1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(!fb->checkValid(ltx, 0, 0, 0));
                REQUIRE(fb->getResultCode() == txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("extra signatures")
        {
            auto acc = root.create("A", 2 * reserve + 2 * fee);
            for_versions_from(13, *app, [&] {
                auto fbXDR = feeBumpUnsigned(acc, root, root, 2 * fee, fee, 1);
                sign(app->getNetworkID(), root,
                     fbXDR.feeBump().tx.innerTx.v1());
                sign(app->getNetworkID(), acc, fbXDR.feeBump());
                sign(app->getNetworkID(), root, fbXDR.feeBump());
                auto fb = TransactionFrameBase::makeTransactionFromWire(
                    app->getNetworkID(), fbXDR);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(!fb->checkValid(ltx, 0, 0, 0));
                REQUIRE(fb->getResultCode() == txBAD_AUTH_EXTRA);
            });
        }

        SECTION("inner transaction invalid, transaction level")
        {
            auto acc = root.create("A", 2 * reserve + 2 * fee);
            for_versions_from(13, *app, [&] {
                auto fbXDR = feeBumpUnsigned(acc, root, root, 2 * fee, fee, 1);
                sign(app->getNetworkID(), acc, fbXDR.feeBump());
                auto fb = TransactionFrameBase::makeTransactionFromWire(
                    app->getNetworkID(), fbXDR);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(!fb->checkValid(ltx, 0, 0, 0));
                REQUIRE(fb->getResultCode() == txFEE_BUMP_INNER_FAILED);
                auto const& fbRes = fb->getResult();
                REQUIRE(fbRes.feeCharged == 2 * fee);
                auto const& innerRes = fbRes.result.innerResultPair().result;
                REQUIRE(innerRes.feeCharged == 0);
                REQUIRE(innerRes.result.code() == txBAD_AUTH);
            });
        }

        SECTION("inner transaction invalid, operation level")
        {
            auto acc = root.create("A", 2 * reserve + 2 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, root, root, 2 * fee,
                                  fee, -1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(!fb->checkValid(ltx, 0, 0, 0));
                REQUIRE(fb->getResultCode() == txFEE_BUMP_INNER_FAILED);
                auto const& fbRes = fb->getResult();
                REQUIRE(fbRes.feeCharged == 2 * fee);
                auto const& innerRes = fbRes.result.innerResultPair().result;
                REQUIRE(innerRes.feeCharged == 0);
                REQUIRE(innerRes.result.code() == txFAILED);
                auto const& payRes =
                    innerRes.result.results()[0].tr().paymentResult();
                REQUIRE(payRes.code() == PAYMENT_MALFORMED);
            });
        }

        SECTION("valid")
        {
            auto acc = root.create("A", 2 * reserve + 2 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, root, root, 2 * fee,
                                  fee, 1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(fb->checkValid(ltx, 0, 0, 0));
                REQUIRE(fb->getResultCode() == txFEE_BUMP_INNER_SUCCESS);
                auto const& fbRes = fb->getResult();
                REQUIRE(fbRes.feeCharged == 2 * fee);
                auto const& innerRes = fbRes.result.innerResultPair().result;
                REQUIRE(innerRes.result.results().size() == 1);
                REQUIRE(innerRes.result.results()[0].code() == opINNER);
                auto const& payRes =
                    innerRes.result.results()[0].tr().paymentResult();
                REQUIRE(payRes.code() == PAYMENT_SUCCESS);
            });
        }
    }

    SECTION("fee processing")
    {
        auto acc = root.create("A", 2 * reserve + 2 * fee);
        for_versions_from(13, *app, [&] {
            auto fb =
                feeBump(app->getNetworkID(), acc, root, root, 2 * fee, fee, 1);
            LedgerTxn ltx(app->getLedgerTxnRoot());
            fb->processFeeSeqNum(ltx, fee);
            auto delta = ltx.getDelta();
            REQUIRE(delta.entry.size() == 1);
            auto gkey = delta.entry.begin()->first;
            REQUIRE(gkey.type() == InternalLedgerEntryType::LEDGER_ENTRY);
            REQUIRE(gkey.ledgerKey().account().accountID == acc.getPublicKey());
            auto entryDelta = delta.entry.begin()->second;
            auto prev = entryDelta.previous->ledgerEntry().data.account();
            auto curr = entryDelta.current->ledgerEntry().data.account();
            REQUIRE(prev.balance == curr.balance + 2 * fee);
        });
    }

    SECTION("apply")
    {
        SECTION("fee source does not exist")
        {
            auto acc = root.create("A", 2 * reserve + 3 * fee);
            closeLedgerOn(*app, 2, 1, 2, 2016);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, root, root, 2 * fee,
                                  fee, 1);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(ltx, 0, 0, 0));
                }
                acc.merge(root);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta meta(2);
                    REQUIRE(fb->apply(*app, ltx, meta));
                    REQUIRE(fb->getResultCode() == txFEE_BUMP_INNER_SUCCESS);
                }
            });
        }

        SECTION("bad signatures")
        {
            auto acc = root.create("A", 2 * reserve + 3 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, root, root, 2 * fee,
                                  fee, 1);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(ltx, 0, 0, 0));
                }
                acc.setOptions(setMasterWeight(0));
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta meta(2);
                    REQUIRE(fb->apply(*app, ltx, meta));
                    REQUIRE(fb->getResultCode() == txFEE_BUMP_INNER_SUCCESS);
                }
            });
        }

        SECTION("insufficient balance")
        {
            auto acc = root.create("A", 2 * reserve + 3 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, root, root, 2 * fee,
                                  fee, 1);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(ltx, 0, 0, 0));
                }
                acc.pay(root, 2 * fee);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta meta(2);
                    REQUIRE(fb->apply(*app, ltx, meta));
                    REQUIRE(fb->getResultCode() == txFEE_BUMP_INNER_SUCCESS);
                }
            });
        }

        SECTION("extra signatures")
        {
            auto acc = root.create("A", 3 * reserve + 4 * fee);
            acc.setOptions(setSigner(makeSigner(root, 1)) | setLowThreshold(2));
            for_versions_from(13, *app, [&] {
                auto fbXDR = feeBumpUnsigned(acc, root, root, 2 * fee, fee, 1);
                sign(app->getNetworkID(), root,
                     fbXDR.feeBump().tx.innerTx.v1());
                sign(app->getNetworkID(), acc, fbXDR.feeBump());
                sign(app->getNetworkID(), root, fbXDR.feeBump());
                auto fb = TransactionFrameBase::makeTransactionFromWire(
                    app->getNetworkID(), fbXDR);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(ltx, 0, 0, 0));
                }

                auto setOptionsTx = acc.tx({setOptions(setLowThreshold(1))});
                setOptionsTx->addSignature(root);
                applyCheck(setOptionsTx, *app);

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta meta(2);
                    REQUIRE(fb->apply(*app, ltx, meta));
                    REQUIRE(fb->getResultCode() == txFEE_BUMP_INNER_SUCCESS);
                }
            });
        }

        SECTION("inner transaction fails, transaction level")
        {
            auto acc = root.create("A", 2 * reserve + 3 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, root, acc, 2 * fee,
                                  fee, 1);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(ltx, 0, 0, 0));
                }

                auto setOptionsOp = setOptions(setMasterWeight(0));
                setOptionsOp.sourceAccount.activate() = toMuxedAccount(root);
                auto setOptionsTx = acc.tx({setOptionsOp});
                setOptionsTx->addSignature(root);
                applyCheck(setOptionsTx, *app);

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta meta(2);
                    REQUIRE(!fb->apply(*app, ltx, meta));
                    REQUIRE(fb->getResultCode() == txFEE_BUMP_INNER_FAILED);
                    auto const& innerRes =
                        fb->getResult().result.innerResultPair().result;
                    REQUIRE(innerRes.feeCharged == 0);
                    REQUIRE(innerRes.result.code() == txBAD_AUTH);
                }
            });
        }

        SECTION("inner transaction fails, operation level")
        {
            auto acc = root.create("A", 2 * reserve + 3 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, root, acc, 2 * fee,
                                  fee, INT64_MAX);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(ltx, 0, 0, 0));
                }
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta meta(2);
                    REQUIRE(!fb->apply(*app, ltx, meta));
                    REQUIRE(fb->getResultCode() == txFEE_BUMP_INNER_FAILED);
                    auto const& innerRes =
                        fb->getResult().result.innerResultPair().result;
                    REQUIRE(innerRes.feeCharged == 0);
                    REQUIRE(innerRes.result.code() == txFAILED);
                    REQUIRE(innerRes.result.results()[0].code() == opINNER);
                    auto const& payRes =
                        innerRes.result.results()[0].tr().paymentResult();
                    REQUIRE(payRes.code() == PAYMENT_LINE_FULL);
                }
            });
        }

        SECTION("one-time signer removal")
        {
            auto acc = root.create("A", 3 * reserve + 3 * fee);
            auto sponsoring = root.create("sponsoring", 3 * reserve);

            auto signerTest = [&](bool isFbSignerSponsored) {
                auto fbXDR = feeBumpUnsigned(acc, root, root, 2 * fee, fee, 1);
                ++fbXDR.feeBump().tx.innerTx.v1().tx.seqNum;

                auto fb = TransactionFrameBase::makeTransactionFromWire(
                    app->getNetworkID(), fbXDR);

                SignerKey txSigner(SIGNER_KEY_TYPE_PRE_AUTH_TX);
                txSigner.preAuthTx() = sha256(
                    xdr::xdr_to_opaque(app->getNetworkID(), ENVELOPE_TYPE_TX,
                                       fbXDR.feeBump().tx.innerTx.v1().tx));
                root.loadSequenceNumber();
                root.setOptions(setSigner(Signer{txSigner, 1}));

                SignerKey fbSigner(SIGNER_KEY_TYPE_PRE_AUTH_TX);
                fbSigner.preAuthTx() = sha256(xdr::xdr_to_opaque(
                    app->getNetworkID(), ENVELOPE_TYPE_TX_FEE_BUMP,
                    fbXDR.feeBump().tx));

                if (isFbSignerSponsored)
                {
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), acc,
                        {sponsoring.op(beginSponsoringFutureReserves(acc)),
                         acc.op(setOptions(setSigner(Signer{fbSigner, 1}))),
                         acc.op(endSponsoringFutureReserves())},
                        {sponsoring});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx->apply(*app, ltx, txm));
                    REQUIRE(tx->getResultCode() == txSUCCESS);

                    checkSponsorship(ltx, acc, fbSigner, 2,
                                     &sponsoring.getPublicKey());
                    ltx.commit();
                }
                else
                {
                    acc.setOptions(setSigner(Signer{fbSigner, 1}));
                }

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(ltx, 0, 0, 0));
                    REQUIRE(fb->getResultCode() == txFEE_BUMP_INNER_SUCCESS);
                }
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta meta(2);
                    REQUIRE(fb->apply(*app, ltx, meta));
                    REQUIRE(fb->getResultCode() == txFEE_BUMP_INNER_SUCCESS);
                    REQUIRE(meta.v2().txChangesBefore.size() ==
                            (isFbSignerSponsored ? 6 : 4));
                    for (auto const& change : meta.v2().txChangesBefore)
                    {
                        if (change.type() == LEDGER_ENTRY_STATE)
                        {
                            auto const& ae = change.state().data.account();
                            // The sponsoring account doesn't have any signers,
                            // but the account can still change due to
                            // sponsorship
                            REQUIRE(
                                (ae.accountID == sponsoring.getPublicKey() ||
                                 ae.signers.size() == 1));
                        }
                        else if (change.type() == LEDGER_ENTRY_UPDATED)
                        {
                            auto const& ae = change.updated().data.account();
                            REQUIRE(ae.signers.empty());
                        }
                    }
                    ltx.commit();
                }

                REQUIRE(getAccountSigners(root, *app).size() == 0);
                REQUIRE(getAccountSigners(acc, *app).size() == 0);

                if (isFbSignerSponsored)
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    checkSponsorship(ltx, acc, 0, nullptr, 0, 2, 0, 0);
                    checkSponsorship(ltx, sponsoring, 0, nullptr, 0, 2, 0, 0);
                }
            };

            SECTION("not sponsored")
            {
                for_versions_from(13, *app, [&] { signerTest(false); });
            }
            SECTION("sponsored")
            {
                for_versions_from(14, *app, [&] { signerTest(true); });
            }
        }
    }
}
