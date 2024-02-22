// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <exception>
#include <fmt/format.h>
#include <variant>

#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "util/GlobalChecks.h"
#include "util/types.h"
#include "util/xdrquery/XDRQueryError.h"
#include "util/xdrquery/XDRQueryEval.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdrpp/marshal.h"
#include "xdrpp/types.h"

namespace xdrquery
{
namespace internal
{
using namespace xdr;
using namespace stellar;

struct XDRFieldResolver
{
    XDRFieldResolver(std::vector<std::string> const& fieldPath, bool validate)
        : mFieldPath(fieldPath)
        , mPathIter(mFieldPath.cbegin())
        , mValidate(validate)
    {
    }

    ResultType const&
    getResult()
    {
        releaseAssert(!mValidate);
        return mResult;
    }

    bool
    isValid() const
    {
        releaseAssert(mValidate);
        return mPathIter == mFieldPath.end();
    }

    template <typename T>
    typename std::enable_if_t<xdr_traits<T>::is_numeric &&
                              !xdr_traits<T>::is_enum>
    operator()(T const& t, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = t;
        }
    }

    // Retrieve enums as their XDR string representation.
    template <typename T>
    typename std::enable_if_t<xdr_traits<T>::is_enum>
    operator()(T const& t, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = std::string(xdr_traits<T>::enum_name(t));
        }
    }

    // Retrieve public keys in standard string representation.
    template <typename T>
    typename std::enable_if_t<std::is_same_v<PublicKey, T>>
    operator()(T const& k, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = stellar::KeyUtils::toStrKey(k);
        }
    }

    // Retrieve SCAddresses in standard string representation.
    template <typename T>
    typename std::enable_if_t<std::is_same_v<SCAddress, T>>
    operator()(T const& k, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            switch (k.type())
            {
            case SC_ADDRESS_TYPE_CONTRACT:
                mResult = stellar::strKey::toStrKey(
                              stellar::strKey::STRKEY_CONTRACT, k.contractId())
                              .value;
                break;
            case SC_ADDRESS_TYPE_ACCOUNT:
                mResult = stellar::KeyUtils::toStrKey(k.accountId());
                break;
            default:
                mResult = "UNKNOWN";
                break;
            }
        }
    }

    template <typename T>
    typename std::enable_if_t<std::is_same_v<Asset, T> ||
                              std::is_same_v<TrustLineAsset, T>>
    operator()(T const& asset, char const* fieldName)
    {
        if (!matchFieldToPath(fieldName))
        {
            return;
        }
        ++mPathIter;
        switch (asset.type())
        {
        case ASSET_TYPE_NATIVE:
            if (mPathIter == mFieldPath.end())
            {
                // If non-leaf field is requested, then we must be looking for
                // non-native asset.
                mResult.emplace().emplace<std::string>() = "NATIVE";
            }
            break;
        case ASSET_TYPE_POOL_SHARE:
            processPoolAsset(asset);
            break;
        case ASSET_TYPE_CREDIT_ALPHANUM4:
        case ASSET_TYPE_CREDIT_ALPHANUM12:
        {
            std::string code;
            if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
            {
                stellar::assetCodeToStr(asset.alphaNum4().assetCode, code);
            }
            else
            {
                stellar::assetCodeToStr(asset.alphaNum12().assetCode, code);
            }

            processString(code, "assetCode");
            (*this)(stellar::getIssuer(asset), "issuer");
            break;
        }
        default:
            mResult = "UNKNOWN";
            break;
        }

        if (mValidate)
        {
            validateAsset(asset, fieldName);
        }
    }

    template <uint32_t N>
    void
    operator()(xstring<N> const& t, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = std::string(t);
        }
    }

    template <uint32_t N>
    void
    operator()(xdr::opaque_vec<N> const& v, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = binToHex(ByteSlice(v.data(), v.size()));
        }
    }

    template <uint32_t N>
    void
    operator()(xdr::opaque_array<N> const& v, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = binToHex(ByteSlice(v.data(), v.size()));
        }
    }

    template <typename T>
    void
    operator()(xdr::pointer<T> const& ptr, char const* fieldName)
    {
        if (ptr)
        {
            archive(*this, *ptr, fieldName);
        }
        else
        {
            if (checkLeafField(fieldName))
            {
                mResult = NullField();
                return;
            }
            if (mValidate && matchFieldToPath(fieldName))
            {
                ++mPathIter;
                // Create an instance of the field for validation.
                T t;
                xdr_traits<T>::save(*this, t);
            }
        }
    }

    template <typename T>
    typename std::enable_if_t<xdr_traits<T>::is_container>
    operator()(T const& t, char const* fieldName)
    {
        if (matchFieldToPath(fieldName))
        {
            throw XDRQueryError(
                fmt::format(FMT_STRING("Array fields are not supported: '{}'."),
                            fieldName));
        }
    }

    template <typename T>
    typename std::enable_if_t<
        xdr_traits<T>::is_union && !std::is_same_v<PublicKey, T> &&
        !std::is_same_v<Asset, T> && !std::is_same_v<TrustLineAsset, T> &&
        !xdr_traits<T>::is_container && !std::is_same_v<SCAddress, T>>
    operator()(T const& t, char const* fieldName)
    {
        if (!matchFieldToPath(fieldName))
        {
            // Archive is first called with an empty 'virtual' XDR field
            // representing the whole struct.
            if (fieldName == nullptr && mPathIter == mFieldPath.begin())
            {
                xdr_traits<T>::save(*this, t);
            }
            return;
        }
        if (++mPathIter == mFieldPath.end())
        {
            throw XDRQueryError("Field path must end with a primitive field.");
        }
        xdr_traits<T>::save(*this, t);
        if (mValidate)
        {
            validateUnion(t, fieldName);
        }
    }

    template <typename T>
    typename std::enable_if_t<
        xdr_traits<T>::is_class && !std::is_same_v<PublicKey, T> &&
        !std::is_same_v<Asset, T> && !std::is_same_v<TrustLineAsset, T> &&
        !xdr_traits<T>::is_union && !xdr_traits<T>::is_container &&
        !std::is_same_v<SCAddress, T>>
    operator()(T const& t, char const* fieldName)
    {
        if (!matchFieldToPath(fieldName))
        {
            // Archive is first called with an empty 'virtual' XDR field
            // representing the whole struct.
            if (fieldName == nullptr && mPathIter == mFieldPath.begin())
            {
                xdr_traits<T>::save(*this, t);
            }
            return;
        }
        if (++mPathIter == mFieldPath.end())
        {
            throw XDRQueryError("Field path must end with a primitive field.");
        }
        xdr_traits<T>::save(*this, t);
    }

  private:
    bool
    matchFieldToPath(char const* fieldName) const
    {
        return fieldName != nullptr && mPathIter != mFieldPath.end() &&
               *mPathIter == fieldName;
    }

    bool
    checkLeafField(char const* fieldName)
    {
        if (!matchFieldToPath(fieldName))
        {
            return false;
        }
        if (++mPathIter != mFieldPath.end())
        {
            throw XDRQueryError(
                fmt::format(FMT_STRING("Encountered leaf field in the middle "
                                       "of the field path: '{}'."),
                            fieldName));
        }
        return true;
    }

    bool
    checkMaybeLeafField(char const* fieldName, bool& isLeaf)
    {
        if (!matchFieldToPath(fieldName))
        {
            return false;
        }
        return ++mPathIter != mFieldPath.end();
    }

    void
    processString(std::string const& s, char const* fieldName)
    {
        if (checkLeafField(fieldName))
        {
            mResult = s;
        }
    }

    void
    processPoolAsset(Asset const& asset)
    {
        throw std::runtime_error("Unexpected asset type for the pool asset.");
    }

    void
    processPoolAsset(TrustLineAsset const& asset)
    {
        (*this)(asset.liquidityPoolID(), "liquidityPoolID");
    }

    template <typename T>
    typename std::enable_if_t<xdr_traits<T>::is_union>
    validateUnion(T const& t, char const* fieldName)
    {
        // The field could have been already matched if it was XDR discriminant.
        if (mPathIter == mFieldPath.end())
        {
            return;
        }
        for (auto const c : t._xdr_case_values())
        {
            auto unionFieldName = xdr_traits<T>::union_field_name(c);
            if (unionFieldName == nullptr || unionFieldName != *mPathIter)
            {
                continue;
            }
            auto tCopy = t;
            tCopy._xdr_discriminant(c, false);
            tCopy._xdr_with_mem_ptr(field_archiver, c, *this, tCopy,
                                    unionFieldName);
            break;
        }
    }

    template <typename T>
    typename std::enable_if_t<std::is_same_v<Asset, T> ||
                              std::is_same_v<TrustLineAsset, T>>
    validateAsset(T const& asset, char const* fieldName)
    {
        // The field could have been already matched if it was a native asset.
        if (mPathIter == mFieldPath.end())
        {
            return;
        }

        // Check if the field is requested from liquidity pool share.
        if constexpr (std::is_same_v<TrustLineAsset, T>)
        {
            checkLeafField("liquidityPoolID");
        }
        // For regular asset we allow 'assetCode' and 'issuer' fields.
        checkLeafField("assetCode");
        checkLeafField("issuer");
    }

    std::vector<std::string> const& mFieldPath;
    std::vector<std::string>::const_iterator mPathIter;
    ResultType mResult;
    bool mValidate = false;
};

} // namespace internal

// Returns the value of the field in `xdrMessage` specified by `fieldPath`.
// When path goes through a union option that is not selected, returns
// std::nullopt.
// When path ends with an optional field and the field is not set,
// returns `NullField`.
// Some types have special overrides, mostly consistent with XDR-to-JSON
// representation:
//   - enums are represented by strings
//   - `AccountID` has a standard string representation ('GXYZ...')
//   - Assets are simplified to {`assetCode`, `issuer`} struct (and
//     `liquidityPoolID` for the pool shares)
//   - Fixed size byte arrays are represented by the hex strings
template <typename T>
ResultType
getXDRField(T const& xdrMessage, std::vector<std::string> const& fieldPath)
{
    internal::XDRFieldResolver resolver(fieldPath, false);
    xdr::xdr_argpack_archive(resolver, xdrMessage);
    return resolver.getResult();
}

// Like `getXDRField`, but throws XDRQueryError when path is not present in XDR
// message (accounting for all the union variants and optional fields).
template <typename T>
ResultType
getXDRFieldValidated(T const& xdrMessage,
                     std::vector<std::string> const& fieldPath)
{
    internal::XDRFieldResolver validator(fieldPath, true);
    xdr::xdr_argpack_archive(validator, xdrMessage);
    if (!validator.isValid())
    {
        throw XDRQueryError(fmt::format(FMT_STRING("Invalid field path: '{}'."),
                                        fmt::join(fieldPath, ".")));
    }
    return getXDRField(xdrMessage, fieldPath);
}

} // namespace xdrquery

namespace xdr
{
template <> struct archive_adapter<xdrquery::internal::XDRFieldResolver>
{
    template <typename T>
    static void
    apply(xdrquery::internal::XDRFieldResolver& ar, T&& t,
          char const* fieldName)
    {
        ar(std::forward<T>(t), fieldName);
    }
};

}
