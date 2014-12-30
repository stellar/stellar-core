#ifndef __BASE58__
#define __BASE58__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <string>
#include <vector>

namespace stellar
{

// Higher-level routines that target the stellar alphabet stellar base58check
// version tags defined here (and a double-sha256 checksum, a la bitcoin); you
// probably want to use these.

typedef enum
{
    VER_NONE = 1,
    VER_NODE_PUBLIC = 122,     // 'n'
    VER_NODE_PRIVATE = 102,    // 'h'
    VER_ACCOUNT_ID = 0,        // 'g'
    VER_ACCOUNT_PUBLIC = 67,   // 'p'
    VER_ACCOUNT_PRIVATE = 101, // 'h'
    VER_SEED = 33              // 's'
} Base58CheckVersionByte;

std::string
toBase58Check(Base58CheckVersionByte ver, std::vector<uint8_t> const& bytes);

std::pair<Base58CheckVersionByte, std::vector<uint8_t>>
fromBase58Check(std::string const& encoded);


// Lower-level helper routines; you probably don't want to use these ones.

std::string
baseEncode(std::string const& alphabet, std::vector<uint8_t> const& bytes);

std::vector<uint8_t>
baseDecode(std::string const& alphabet, std::string const& encoded);

std::string
baseCheckEncode(std::string const& alphabet, uint8_t ver, std::vector<uint8_t> const& bytes);

std::pair<uint8_t, std::vector<uint8_t>>
baseCheckDecode(std::string const& alphabet, std::string const& encoded);

extern const std::string bitcoinBase58Alphabet;
extern const std::string stellarBase58Alphabet;

}

#endif
