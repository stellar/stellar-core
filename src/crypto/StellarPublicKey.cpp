// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "StellarPublicKey.h"
#include <sodium.h>

namespace stellar
{

	StellarPublicKey::StellarPublicKey()
	{
		mIsValid = false;
	}

	StellarPublicKey::StellarPublicKey(const Blob& publicKey, RippleAddress::VersionEncoding type)
	{
		SetData(type, publicKey);
	}

	StellarPublicKey::StellarPublicKey(const std::string& base58Key, RippleAddress::VersionEncoding type)
	{
		setKey(base58Key, type);
	}

	bool StellarPublicKey::setKey(const std::string& base58Key, RippleAddress::VersionEncoding type)
	{
		mIsValid = SetString(base58Key, RippleAddress::VER_NODE_PUBLIC, Base58::getRippleAlphabet());
		return(mIsValid);
	}

	// JED: seems like we shouldn't need this
	void StellarPublicKey::clear()
	{
		nVersion = RippleAddress::VER_NONE;
		vchData.clear();
		mIsValid = false;
	}

	uint160 StellarPublicKey::getAccountID() const
	{
		return Hash160(vchData);
	}

	// create accountID from this seed and base58 encode it
	std::string StellarPublicKey::base58AccountID() const
	{
		uint160 account = getAccountID();

		Blob vch(1, RippleAddress::VER_ACCOUNT_ID);

		vch.insert(vch.end(), account.begin(), account.end());

		return Base58::encodeWithCheck(vch);

	}

	std::string StellarPublicKey::base58Key() const
	{
		return(ToString());
	}


	bool StellarPublicKey::verifySignature(stellarxdr::uint256 const& hash, const std::string& strSig) const
	{
		Blob vchSig(strSig.begin(), strSig.end());
		return(verifySignature(hash, vchSig));
	}

	bool StellarPublicKey::verifySignature(stellarxdr::uint256 const& hash, Blob const& vchSig) const
	{
		if (vchData.size() != crypto_sign_PUBLICKEYBYTES
			|| vchSig.size() != crypto_sign_BYTES)
			throw std::runtime_error("bad inputs to verifySignature");
		/*
		unsigned char signed_buf[crypto_sign_BYTES + hash.bytes];
		memcpy (signed_buf, vchSig.data(), crypto_sign_BYTES);
		memcpy (signed_buf+crypto_sign_BYTES, hash.data(), hash.bytes);

		unsigned char ignored_buf[hash.bytes];
		unsigned long long ignored_len;
		return crypto_sign_open (ignored_buf, &ignored_len,
		signed_buf, sizeof (signed_buf),
		vchData.data()) == 0;
		*/

		unsigned char signed_buf[crypto_sign_BYTES + hash.bytes];
		memcpy(signed_buf, vchSig.data(), crypto_sign_BYTES);
		memcpy(signed_buf + crypto_sign_BYTES, hash.data(), hash.bytes);


		unsigned char ignored_buf[hash.bytes];
		unsigned long long ignored_len;
		return crypto_sign_open(ignored_buf, &ignored_len,
			signed_buf, sizeof (signed_buf),
			vchData.data()) == 0;


	}

	


}
