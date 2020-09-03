// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TxSimUtils.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SignerKey.h"
#include "invariant/test/InvariantTestUtils.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{
namespace txsimulation
{

static void
replaceIssuer(Asset& asset, uint32_t partition)
{
    switch (asset.type())
    {
    case ASSET_TYPE_CREDIT_ALPHANUM4:
        mutateScaledAccountID(asset.alphaNum4().issuer, partition);
        break;
    case ASSET_TYPE_CREDIT_ALPHANUM12:
        mutateScaledAccountID(asset.alphaNum12().issuer, partition);
        break;
    case ASSET_TYPE_NATIVE:
        // nothing to do for native assets
        break;
    default:
        abort();
    }
}

SecretKey
generateScaledSecret(AccountID const& key, uint32_t partition)
{
    if (partition == 0)
    {
        throw std::runtime_error(
            "Must call generateScaledSecret on simulated (non-zero) partition");
    }
    return SecretKey::fromSeed(
        sha256(KeyUtils::toStrKey(key) + std::to_string(partition)));
}

SecretKey
generateScaledSecret(MuxedAccount const& key, uint32_t partition)
{
    return generateScaledSecret(toAccountID(key), partition);
}

int64_t
generateScaledOfferID(int64_t offerId, uint32_t partition)
{
    if (partition == 0 || offerId == 0)
    {
        return offerId;
    }

    // For the purpose of the simulation, assume offer id is capped at INT32_MAX
    if (offerId > INT32_MAX)
    {
        throw std::runtime_error(
            "Invalid offer id, simulation will not work correctly");
    }

    uint64 offset = static_cast<uint64_t>(partition) << 32u;
    uint64_t newId = offset | static_cast<uint64_t>(offerId);

    return newId;
}

int64_t
generateScaledOfferID(OperationResult const& result, uint32_t partition)
{
    if (result.code() != opINNER)
    {
        // This will produce txINTERNAL_ERROR on apply
        throw std::runtime_error(
            "generateScaledOfferID: invalid OperationResult");
    }

    // Note: assume here operation passed. If it did not, simulated transaction
    // will fail with txINTERNAL_ERROR (whereas op in production failed with
    // some other code)
    int64_t offerID;
    switch (result.tr().type())
    {
    case MANAGE_SELL_OFFER:
        offerID =
            result.tr().manageSellOfferResult().success().offer.offer().offerID;
        break;
    case MANAGE_BUY_OFFER:
        offerID =
            result.tr().manageBuyOfferResult().success().offer.offer().offerID;
        break;
    case CREATE_PASSIVE_SELL_OFFER:
        offerID = result.tr()
                      .createPassiveSellOfferResult()
                      .success()
                      .offer.offer()
                      .offerID;
        break;
    default:
        throw std::runtime_error(
            "Invalid OperationResult: must be manage offer");
    }
    return generateScaledOfferID(offerID, partition);
}

Hash
generateScaledClaimableBalanceID(OperationResult const& result,
                                 uint32_t partition)
{
    if (result.code() != opINNER)
    {
        // This will produce txINTERNAL_ERROR on apply
        throw std::runtime_error(
            "generateScaledOfferID: invalid OperationResult");
    }

    if (result.tr().type() != CREATE_CLAIMABLE_BALANCE)
    {
        throw std::runtime_error(
            "Invalid OperationResult: must be CREATE_CLAIMABLE_BALANCE");
    }

    auto const& balanceID =
        result.tr().createClaimableBalanceResult().balanceID().v0();
    return generateScaledClaimableBalanceID(balanceID, partition);
}

Hash
generateScaledClaimableBalanceID(Hash const& balanceID, uint32_t partition)
{
    auto scaledID = balanceID;

    // XOR balanceID and partition
    auto i = scaledID.size() - 1;
    while (partition > 0 && i >= 0)
    {
        scaledID[i] ^= (partition & 0xFF);
        partition >>= 8;
        --i;
    }

    return scaledID;
}

SignerKey
generateScaledEd25519Signer(Signer const& signer, uint32_t partition)
{
    assert(signer.key.type() == SIGNER_KEY_TYPE_ED25519);
    auto pubKey = KeyUtils::convertKey<PublicKey>(signer.key);
    auto newPubKey = generateScaledSecret(pubKey, partition).getPublicKey();
    return KeyUtils::convertKey<SignerKey>(newPubKey);
}

SecretKey
mutateScaledAccountID(AccountID& acc, uint32_t partition)
{
    auto key = generateScaledSecret(acc, partition);
    acc = key.getPublicKey();
    return key;
}

SecretKey
mutateScaledAccountID(MuxedAccount& acc, uint32_t partition)
{
    auto key = generateScaledSecret(acc, partition);
    acc = toMuxedAccount(key.getPublicKey());
    return key;
}

void
generateScaledLiveEntries(std::vector<LedgerEntry>& entries,
                          std::vector<LedgerEntry> const& oldEntries,
                          uint32_t partition)
{
    entries.clear();

    for (auto const& le : oldEntries)
    {
        LedgerEntry newEntry = le;
        if (newEntry.ext.v() == 1 && newEntry.ext.v1().sponsoringID)
        {
            mutateScaledAccountID(*newEntry.ext.v1().sponsoringID, partition);
        }

        switch (le.data.type())
        {
        case ACCOUNT:
        {
            auto& ae = newEntry.data.account();
            mutateScaledAccountID(ae.accountID, partition);
            ae.signers.clear();

            if (ae.ext.v() == 1 && ae.ext.v1().ext.v() == 2)
            {
                ae.ext.v1().ext.v2().signerSponsoringIDs.clear();
            }

            for (size_t i = 0; i < le.data.account().signers.size(); ++i)
            {
                auto const& signer = le.data.account().signers[i];
                if (signer.key.type() == SIGNER_KEY_TYPE_ED25519)
                {
                    ae.signers.emplace_back(
                        generateScaledEd25519Signer(signer, partition),
                        signer.weight);

                    if (ae.ext.v() == 1 && ae.ext.v1().ext.v() == 2)
                    {
                        // pull the id from le
                        auto idToScale = le.data.account()
                                             .ext.v1()
                                             .ext.v2()
                                             .signerSponsoringIDs[i];
                        if (idToScale)
                        {
                            mutateScaledAccountID(*idToScale, partition);
                        }

                        ae.ext.v1().ext.v2().signerSponsoringIDs.emplace_back(
                            idToScale);
                    }
                }
            }

            InvariantTestUtils::normalizeSigners(newEntry.data.account());
            break;
        }
        case TRUSTLINE:
            mutateScaledAccountID(newEntry.data.trustLine().accountID,
                                  partition);
            replaceIssuer(newEntry.data.trustLine().asset, partition);
            break;
        case DATA:
            mutateScaledAccountID(newEntry.data.data().accountID, partition);
            break;
        case OFFER:
            mutateScaledAccountID(newEntry.data.offer().sellerID, partition);
            newEntry.data.offer().offerID =
                generateScaledOfferID(le.data.offer().offerID, partition);
            replaceIssuer(newEntry.data.offer().buying, partition);
            replaceIssuer(newEntry.data.offer().selling, partition);
            break;
        case CLAIMABLE_BALANCE:
        {
            replaceIssuer(newEntry.data.claimableBalance().asset, partition);
            newEntry.data.claimableBalance().balanceID.v0() =
                generateScaledClaimableBalanceID(
                    newEntry.data.claimableBalance().balanceID.v0(), partition);

            auto& claimants = newEntry.data.claimableBalance().claimants;
            for (auto& claimant : claimants)
            {
                mutateScaledAccountID(claimant.v0().destination, partition);
            }
            break;
        }
        default:
            abort();
        }
        entries.emplace_back(newEntry);
    }
}

void
generateScaledDeadEntries(std::vector<LedgerKey>& dead,
                          std::vector<LedgerKey> const& oldKeys,
                          uint32_t partition)
{
    for (auto const& lk : oldKeys)
    {
        LedgerKey newKey = lk;

        switch (lk.type())
        {
        case ACCOUNT:
            mutateScaledAccountID(newKey.account().accountID, partition);
            break;
        case TRUSTLINE:
            mutateScaledAccountID(newKey.trustLine().accountID, partition);
            replaceIssuer(newKey.trustLine().asset, partition);
            break;
        case DATA:
            mutateScaledAccountID(newKey.data().accountID, partition);
            break;
        case OFFER:
            mutateScaledAccountID(newKey.offer().sellerID, partition);
            newKey.offer().offerID =
                generateScaledOfferID(lk.offer().offerID, partition);
            break;
        case CLAIMABLE_BALANCE:
            newKey.claimableBalance().balanceID.v0() =
                generateScaledClaimableBalanceID(
                    newKey.claimableBalance().balanceID.v0(), partition);
            break;
        default:
            abort();
        }
        dead.emplace_back(newKey);
    }
}

void
mutateScaledOperation(Operation& op, uint32_t partition)
{
    // Update sourceAccount, if present
    if (op.sourceAccount)
    {
        auto opKey = generateScaledSecret(*op.sourceAccount, partition);
        op.sourceAccount.activate() = toMuxedAccount(opKey.getPublicKey());
    }

    switch (op.body.type())
    {
    case CREATE_ACCOUNT:
        mutateScaledAccountID(op.body.createAccountOp().destination, partition);
        break;
    case PAYMENT:
        mutateScaledAccountID(op.body.paymentOp().destination, partition);
        replaceIssuer(op.body.paymentOp().asset, partition);
        break;
    case PATH_PAYMENT_STRICT_RECEIVE:
        replaceIssuer(op.body.pathPaymentStrictReceiveOp().sendAsset,
                      partition);
        replaceIssuer(op.body.pathPaymentStrictReceiveOp().destAsset,
                      partition);
        mutateScaledAccountID(op.body.pathPaymentStrictReceiveOp().destination,
                              partition);
        for (auto& asset : op.body.pathPaymentStrictReceiveOp().path)
        {
            replaceIssuer(asset, partition);
        }
        break;
    case PATH_PAYMENT_STRICT_SEND:
        replaceIssuer(op.body.pathPaymentStrictSendOp().sendAsset, partition);
        replaceIssuer(op.body.pathPaymentStrictSendOp().destAsset, partition);
        mutateScaledAccountID(op.body.pathPaymentStrictSendOp().destination,
                              partition);
        for (auto& asset : op.body.pathPaymentStrictSendOp().path)
        {
            replaceIssuer(asset, partition);
        }
        break;
    case MANAGE_SELL_OFFER:
        op.body.manageSellOfferOp().offerID = generateScaledOfferID(
            op.body.manageSellOfferOp().offerID, partition);
        replaceIssuer(op.body.manageSellOfferOp().selling, partition);
        replaceIssuer(op.body.manageSellOfferOp().buying, partition);
        break;
    case CREATE_PASSIVE_SELL_OFFER:
        replaceIssuer(op.body.createPassiveSellOfferOp().selling, partition);
        replaceIssuer(op.body.createPassiveSellOfferOp().buying, partition);
        break;
    case SET_OPTIONS:
        if (op.body.setOptionsOp().signer)
        {
            Signer& signer = *op.body.setOptionsOp().signer;
            if (signer.key.type() == SIGNER_KEY_TYPE_ED25519)
            {
                signer = Signer{generateScaledEd25519Signer(signer, partition),
                                signer.weight};
            }
        }
        break;
    case CHANGE_TRUST:
        replaceIssuer(op.body.changeTrustOp().line, partition);
        break;
    case ALLOW_TRUST:
        mutateScaledAccountID(op.body.allowTrustOp().trustor, partition);
        break;
    case ACCOUNT_MERGE:
        mutateScaledAccountID(op.body.destination(), partition);
        break;
    case MANAGE_BUY_OFFER:
        op.body.manageBuyOfferOp().offerID = generateScaledOfferID(
            op.body.manageBuyOfferOp().offerID, partition);
        replaceIssuer(op.body.manageBuyOfferOp().selling, partition);
        replaceIssuer(op.body.manageBuyOfferOp().buying, partition);
        break;
    case CREATE_CLAIMABLE_BALANCE:
    {
        auto& claimants = op.body.createClaimableBalanceOp().claimants;
        for (auto& claimant : claimants)
        {
            mutateScaledAccountID(claimant.v0().destination, partition);
        }

        replaceIssuer(op.body.createClaimableBalanceOp().asset, partition);
        break;
    }
    case CLAIM_CLAIMABLE_BALANCE:
        op.body.claimClaimableBalanceOp().balanceID.v0() =
            generateScaledClaimableBalanceID(
                op.body.claimClaimableBalanceOp().balanceID.v0(), partition);
        break;
    case INFLATION:
    case MANAGE_DATA:
    case BUMP_SEQUENCE:
        // Note: don't care about inflation
        break;
    default:
        abort();
    }
}
}
}
