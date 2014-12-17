// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC


#include "EdKeyPair.h"
#include <sodium.h>

namespace stellar {

	EdKeyPair::EdKeyPair()
	{
		mPublicKey.resize(crypto_sign_ed25519_PUBLICKEYBYTES);
		mPrivateKey.resize(crypto_sign_ed25519_SECRETKEYBYTES);
	}

	EdKeyPair::EdKeyPair(const Blob& seed) : EdKeyPair()
	{
		setSeed(seed);
	}

	EdKeyPair::EdKeyPair(const stellarxdr::uint256& passPhrase) : EdKeyPair()
	{
		if (crypto_sign_seed_keypair(&(mPublicKey[0]), &mPrivateKey[0], passPhrase.begin()) == -1)
		{
			throw key_error("EdKeyPair::EdKeyPair(const uint128& passPhrase) failed");
		}
	}

	void EdKeyPair::setSeed(const Blob& seed)
	{
		if (seed.size() != 32) throw key_error("EdKeyPair::EdKeyPair wrong sized blob");

		if (crypto_sign_seed_keypair(&(mPublicKey[0]), &mPrivateKey[0], &seed[0]) == -1)
		{
			throw key_error("EdKeyPair::EdKeyPair(const uint128& seed) failed");
		}
	}


	// <-- seed
	uint256 EdKeyPair::passPhraseToKey(const std::string& passPhrase)
	{
		// TODO: Should we use the libsodium hashing here?
		Serializer s;

		s.addRaw(passPhrase);
		uint256 hash256 = s.getSHA512Half();
		s.secureErase();

		return hash256;
	}

	void EdKeyPair::getPrivateKeyU(stellarxdr::uint256& privKey)
	{
		memcpy(privKey.begin(), &mPrivateKey[0], crypto_sign_ed25519_SECRETKEYBYTES);
	}

}
