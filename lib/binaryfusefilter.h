#ifndef BINARYFUSEFILTER_H
#define BINARYFUSEFILTER_H
#include <cstdint>
#include <cstdio>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdexcept>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <vector>

#include "util/siphash.h"
#include "xdr/Stellar-types.h"

#include <Tracy.hpp>
#include <sodium.h>

typedef std::array<uint8_t, crypto_shorthash_KEYBYTES> binary_fuse_seed_t;

#ifndef XOR_MAX_ITERATIONS
#define XOR_MAX_ITERATIONS \
    1000000 // probability of success should always be > 0.5, so with this many
            // iterations, it is essentially impossible to fail
#endif

/**
 * We start with a few utilities.
 ***/
static inline uint64_t
sip_hash24(uint64_t key, binary_fuse_seed_t const& seed)
{
    SipHash24 hasher(seed.data());
    hasher.update(reinterpret_cast<unsigned char*>(&key), sizeof(key));
    return hasher.digest();
}
static inline uint64_t
binary_fuse_rotl64(uint64_t n, unsigned int c)
{
    return (n << (c & 63)) | (n >> ((0u - c) & 63));
}
static inline uint32_t
binary_fuse_reduce(uint32_t hash, uint32_t n)
{
    // http://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
    return (uint32_t)(((uint64_t)hash * n) >> 32);
}

/**
 * We need a decent random number generator.
 **/

// returns random number, modifies the seed
static inline uint64_t
binary_fuse_rng_splitmix64(uint64_t* seed)
{
    uint64_t z = (*seed += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

// #ifdefs adapted from:
//  https://stackoverflow.com/a/50958815
#ifdef __SIZEOF_INT128__ // compilers supporting __uint128, e.g., gcc, clang
static inline uint64_t
binary_fuse_mulhi(uint64_t a, uint64_t b)
{
    return ((__uint128_t)a * b) >> 64;
}
#elif defined(_M_X64) || defined(_MARM64) // MSVC
#include <intrin.h>
static inline uint64_t
binary_fuse_mulhi(uint64_t a, uint64_t b)
{
    return __umulh(a, b);
}
#elif defined(_M_IA64)                    // also MSVC
static inline uint64_t
binary_fuse_mulhi(uint64_t a, uint64_t b)
{
    unsigned __int64 hi;
    (void)_umul128(a, b, &hi);
    return hi;
}
#else // portable implementation using uint64_t
static inline uint64_t
binary_fuse_mulhi(uint64_t a, uint64_t b)
{
    // Adapted from:
    //  https://stackoverflow.com/a/51587262

    /*
          This is implementing schoolbook multiplication:

                  a1 a0
          X       b1 b0
          -------------
                     00  LOW PART
          -------------
                  00
               10 10     MIDDLE PART
          +       01
          -------------
               01
          + 11 11        HIGH PART
          -------------
    */

    const uint64_t a0 = (uint32_t)a;
    const uint64_t a1 = a >> 32;
    const uint64_t b0 = (uint32_t)b;
    const uint64_t b1 = b >> 32;
    const uint64_t p11 = a1 * b1;
    const uint64_t p01 = a0 * b1;
    const uint64_t p10 = a1 * b0;
    const uint64_t p00 = a0 * b0;

    // 64-bit product + two 32-bit values
    const uint64_t middle = p10 + (p00 >> 32) + (uint32_t)p01;

    /*
      Proof that 64-bit products can accumulate two more 32-bit values
      without overflowing:

      Max 32-bit value is 2^32 - 1.
      PSum = (2^32-1) * (2^32-1) + (2^32-1) + (2^32-1)
           = 2^64 - 2^32 - 2^32 + 1 + 2^32 - 1 + 2^32 - 1
           = 2^64 - 1
      Therefore the high half below cannot overflow regardless of input.
    */

    // high half
    return p11 + (middle >> 32) + (p01 >> 32);

    // low half (which we don't care about, but here it is)
    // (middle << 32) | (uint32_t) p00;
}
#endif

static inline uint32_t
binary_fuse_calculate_segment_length(uint32_t arity, uint32_t size)
{
    // These parameters are very sensitive. Replacing 'floor' by 'round' can
    // substantially affect the construction time.
    if (arity == 3)
    {
        return ((uint32_t)1)
               << (int)(floor(log((double)(size)) / log(3.33) + 2.25));
    }
    else if (arity == 4)
    {
        return ((uint32_t)1)
               << (int)(floor(log((double)(size)) / log(2.91) - 0.5));
    }
    else
    {
        return 65536;
    }
}

static inline double
binary_fuse_max(double a, double b)
{
    if (a < b)
    {
        return b;
    }
    return a;
}

static inline double
binary_fuse_calculate_size_factor(uint32_t arity, uint32_t size)
{
    if (arity == 3)
    {
        return binary_fuse_max(1.125, 0.875 + 0.25 * log(1000000.0) /
                                                  log((double)size));
    }
    else if (arity == 4)
    {
        return binary_fuse_max(1.075, 0.77 + 0.305 * log(600000.0) /
                                                 log((double)size));
    }
    else
    {
        return 2.0;
    }
}

static inline uint8_t
binary_fuse_mod3(uint8_t x)
{
    return x > 2 ? x - 3 : x;
}

//////////////////
// fuseT
//////////////////

static inline uint64_t
binary_fuse_fingerprint(uint64_t hash)
{
    return hash ^ (hash >> 32);
}

template <typename T> class binary_fuse_t
{
    static_assert(std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> ||
                      std::is_same_v<T, uint32_t>,
                  "Binary Fuse Filter only supports 8, 16, or 32 bit width");

  private:
    binary_fuse_seed_t Seed;
    uint32_t SegmentLength;
    uint32_t SegmentLengthMask;
    uint32_t SegmentCount;
    uint32_t SegmentCountLength;
    uint32_t ArrayLength;
    std::vector<T> Fingerprints;

    struct binary_hashes_t
    {
        uint32_t h0;
        uint32_t h1;
        uint32_t h2;
    };

    binary_hashes_t
    hash_batch(uint64_t hash) const
    {
        uint64_t hi = binary_fuse_mulhi(hash, SegmentCountLength);
        binary_hashes_t ans;
        ans.h0 = (uint32_t)hi;
        ans.h1 = ans.h0 + SegmentLength;
        ans.h2 = ans.h1 + SegmentLength;
        ans.h1 ^= (uint32_t)(hash >> 18) & SegmentLengthMask;
        ans.h2 ^= (uint32_t)(hash)&SegmentLengthMask;
        return ans;
    }

    uint32_t
    binary_fuse_hash(int index, uint64_t hash) const
    {
        uint64_t h = binary_fuse_mulhi(hash, SegmentCountLength);
        h += index * SegmentLength;
        // keep the lower 36 bits
        uint64_t hh = hash & ((1ULL << 36) - 1);
        // index 0: right shift by 36; index 1: right shift by 18; index 2: no
        // shift
        h ^= (size_t)((hh >> (36 - 18 * index)) & SegmentLengthMask);
        return h;
    }

    // Construct the filter, returns true on success, false on failure.
    // The algorithm fails when there is insufficient memory.
    // For best performance, the caller should ensure that there are not
    // too many duplicated keys.
    // While highly improbable, it is possible that the population fails, at
    // which point the seed must be rotated. keys will be sorted and duplicates
    // removed if any duplicate keys exist
    [[nodiscard]] bool
    populate(std::vector<uint64_t>& keys, binary_fuse_seed_t rngSeed)
    {
        ZoneScoped;
        if (keys.size() > std::numeric_limits<uint32_t>::max())
        {
            throw std::runtime_error("size should be at most 2^32");
        }

        uint32_t size = keys.size();
        ZoneValue(static_cast<int64_t>(size));

        Seed = rngSeed;

        std::vector<uint64_t> reverseOrder(size + 1);
        uint32_t capacity = ArrayLength;

        std::vector<uint32_t> alone(capacity);
        std::vector<uint8_t> t2count(capacity);
        std::vector<uint8_t> reverseH(size);
        std::vector<uint64_t> t2hash(capacity);

        uint32_t blockBits = 1;
        while (((uint32_t)1 << blockBits) < SegmentCount)
        {
            blockBits += 1;
        }
        uint32_t block = (1ul << blockBits);

        std::vector<uint32_t> startPos(block);
        std::vector<uint32_t> h012(5);

        reverseOrder.at(size) = 1;
        for (int loop = 0; true; ++loop)
        {
            if (loop + 1 > XOR_MAX_ITERATIONS)
            {
                // The probability of this happening is lower than the
                // the cosmic-ray probability (i.e., a cosmic ray corrupts your
                // system).
                return false;
            }

            for (uint32_t i = 0; i < block; i++)
            {
                // important : i * size would overflow as a 32-bit number in
                // some cases.
                startPos.at(i) = ((uint64_t)i * size) >> blockBits;
            }

            uint64_t maskblock = block - 1;
            for (uint32_t i = 0; i < size; i++)
            {
                uint64_t hash = sip_hash24(keys.at(i), Seed);
                uint64_t segment_index = hash >> (64 - blockBits);
                while (reverseOrder.at(startPos.at(segment_index)) != 0)
                {
                    segment_index++;
                    segment_index &= maskblock;
                }
                reverseOrder.at(startPos.at(segment_index)) = hash;
                startPos.at(segment_index)++;
            }
            int error = 0;
            uint32_t duplicates = 0;
            for (uint32_t i = 0; i < size; i++)
            {
                uint64_t hash = reverseOrder.at(i);
                uint32_t h0 = binary_fuse_hash(0, hash);
                t2count.at(h0) += 4;
                t2hash.at(h0) ^= hash;
                uint32_t h1 = binary_fuse_hash(1, hash);
                t2count.at(h1) += 4;
                t2count.at(h1) ^= 1;
                t2hash.at(h1) ^= hash;
                uint32_t h2 = binary_fuse_hash(2, hash);
                t2count.at(h2) += 4;
                t2hash.at(h2) ^= hash;
                t2count.at(h2) ^= 2;
                if ((t2hash.at(h0) & t2hash.at(h1) & t2hash.at(h2)) == 0)
                {
                    if (((t2hash.at(h0) == 0) && (t2count.at(h0) == 8)) ||
                        ((t2hash.at(h1) == 0) && (t2count.at(h1) == 8)) ||
                        ((t2hash.at(h2) == 0) && (t2count.at(h2) == 8)))
                    {
                        duplicates += 1;
                        t2count.at(h0) -= 4;
                        t2hash.at(h0) ^= hash;
                        t2count.at(h1) -= 4;
                        t2count.at(h1) ^= 1;
                        t2hash.at(h1) ^= hash;
                        t2count.at(h2) -= 4;
                        t2count.at(h2) ^= 2;
                        t2hash.at(h2) ^= hash;
                    }
                }
                error = (t2count.at(h0) < 4) ? 1 : error;
                error = (t2count.at(h1) < 4) ? 1 : error;
                error = (t2count.at(h2) < 4) ? 1 : error;
            }
            if (error)
            {
                std::fill(reverseOrder.begin(), reverseOrder.end(), 0);
                std::fill(t2count.begin(), t2count.end(), 0);
                std::fill(t2hash.begin(), t2hash.end(), 0);

                // Rotate seed deterministically
                auto seedIndex = loop / crypto_shorthash_KEYBYTES;

                // Seed is a carray of size crypto_shorthash_KEYBYTES, can't
                // segfault
                Seed[seedIndex]++;
            }

            // End of key addition
            uint32_t Qsize = 0;
            // Add sets with one key to the queue.
            for (uint32_t i = 0; i < capacity; i++)
            {
                alone.at(Qsize) = i;
                Qsize += ((t2count.at(i) >> 2) == 1) ? 1 : 0;
            }
            uint32_t stacksize = 0;
            while (Qsize > 0)
            {
                Qsize--;
                uint32_t index = alone.at(Qsize);
                if ((t2count.at(index) >> 2) == 1)
                {
                    uint64_t hash = t2hash.at(index);

                    // h012.at(0) = binary_fuse16_hash(0, hash);
                    h012.at(1) = binary_fuse_hash(1, hash);
                    h012.at(2) = binary_fuse_hash(2, hash);
                    h012.at(3) = binary_fuse_hash(0, hash); // == h012.at(0);
                    h012.at(4) = h012.at(1);
                    uint8_t found = t2count.at(index) & 3;
                    reverseH.at(stacksize) = found;
                    reverseOrder.at(stacksize) = hash;
                    stacksize++;
                    uint32_t other_index1 = h012.at(found + 1);
                    alone.at(Qsize) = other_index1;
                    Qsize += ((t2count.at(other_index1) >> 2) == 2 ? 1 : 0);

                    t2count.at(other_index1) -= 4;
                    t2count.at(other_index1) ^= binary_fuse_mod3(found + 1);
                    t2hash.at(other_index1) ^= hash;

                    uint32_t other_index2 = h012.at(found + 2);
                    alone.at(Qsize) = other_index2;
                    Qsize += ((t2count.at(other_index2) >> 2) == 2 ? 1 : 0);
                    t2count.at(other_index2) -= 4;
                    t2count.at(other_index2) ^= binary_fuse_mod3(found + 2);
                    t2hash.at(other_index2) ^= hash;
                }
            }
            if (stacksize + duplicates == size)
            {
                // success
                size = stacksize;
                break;
            }
            else if (duplicates > 0)
            {
                // Sort keys and remove duplicates
                std::sort(keys.begin(), keys.end());
                keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
                size = keys.size();
            }

            // Reset everything except for the last entry in reverseOrder
            std::fill_n(reverseOrder.begin(), size, 0);
            std::fill(t2count.begin(), t2count.end(), 0);
            std::fill(t2hash.begin(), t2hash.end(), 0);

            // Rotate seed deterministically
            auto seedIndex = loop / crypto_shorthash_KEYBYTES;
            Seed[seedIndex]++;
        }

        for (uint32_t i = size - 1; i < size; i--)
        {
            // the hash of the key we insert next
            uint64_t hash = reverseOrder.at(i);
            T xor2 = binary_fuse_fingerprint(hash);
            uint8_t found = reverseH.at(i);
            h012.at(0) = binary_fuse_hash(0, hash);
            h012.at(1) = binary_fuse_hash(1, hash);
            h012.at(2) = binary_fuse_hash(2, hash);
            h012.at(3) = h012.at(0);
            h012.at(4) = h012.at(1);
            Fingerprints.at(h012.at(found)) = xor2 ^ Fingerprints.at(h012.at(found + 1)) ^
                                        Fingerprints.at(h012.at(found + 2));
        }

        return true;
    }

  public:
    // allocate enough capacity for a set containing up to 'size' elements
    // size should be at least 2.
    explicit binary_fuse_t(uint32_t size, std::vector<uint64_t>& keys,
                           binary_fuse_seed_t rngSeed)
    {
        if (size < 2)
        {
            throw std::runtime_error("size should be at least 2");
        }

        uint32_t arity = 3;
        SegmentLength = binary_fuse_calculate_segment_length(arity, size);
        if (SegmentLength > 262144)
        {
            SegmentLength = 262144;
        }
        SegmentLengthMask = SegmentLength - 1;
        double sizeFactor = binary_fuse_calculate_size_factor(arity, size);
        uint32_t capacity = (uint32_t)(round((double)size * sizeFactor));
        uint32_t initSegmentCount =
            (capacity + SegmentLength - 1) / SegmentLength - (arity - 1);
        ArrayLength = (initSegmentCount + arity - 1) * SegmentLength;
        SegmentCount = (ArrayLength + SegmentLength - 1) / SegmentLength;
        if (SegmentCount <= arity - 1)
        {
            SegmentCount = 1;
        }
        else
        {
            SegmentCount = SegmentCount - (arity - 1);
        }
        ArrayLength = (SegmentCount + arity - 1) * SegmentLength;
        SegmentCountLength = SegmentCount * SegmentLength;
        Fingerprints.resize(ArrayLength);

        if (!populate(keys, rngSeed))
        {
            throw std::runtime_error("BinaryFuseFilter failed to populate");
        }
    }

    explicit binary_fuse_t(stellar::SerializedBinaryFuseFilter const& xdrFilter)
        : SegmentLength(xdrFilter.segmentLength)
        , SegmentLengthMask(xdrFilter.segementLengthMask)
        , SegmentCount(xdrFilter.segmentCount)
        , SegmentCountLength(xdrFilter.segmentCountLength)
        , ArrayLength(xdrFilter.fingerprintLength)
    {
        std::copy(xdrFilter.filterSeed.seed.begin(),
                  xdrFilter.filterSeed.seed.end(), Seed.begin());

        // Convert vector<uint8_t> to vector<T>
        Fingerprints.reserve(ArrayLength);
        for (size_t elem = 0; elem < ArrayLength; ++elem)
        {
            T value = 0;
            auto pos = elem * sizeof(T);

            for (auto byte_i = 0; byte_i < sizeof(T); ++byte_i)
            {
                value |= static_cast<T>(xdrFilter.fingerprints.at(pos + byte_i))
                         << (byte_i * 8);
            }

            Fingerprints.push_back(value);
        }
    }

    // Report if the key is in the set, with false positive rate.
    bool
    contain(uint64_t key) const
    {
        ZoneScoped;
        uint64_t hash = sip_hash24(key, Seed);
        T f = binary_fuse_fingerprint(hash);
        binary_hashes_t hashes = hash_batch(hash);
        f ^= Fingerprints.at(hashes.h0) ^ Fingerprints.at(hashes.h1) ^
             Fingerprints.at(hashes.h2);
        return f == 0;
    }

    // report memory usage
    size_t
    size_in_bytes() const
    {
        return ArrayLength * sizeof(T) + sizeof(*this);
    }

    void
    copyTo(stellar::SerializedBinaryFuseFilter& xdrFilter) const
    {
        if constexpr (std::is_same<T, uint8_t>::value)
        {
            xdrFilter.type = stellar::BINARY_FUSE_FILTER_8_BIT;
        }
        else if constexpr (std::is_same<T, uint16_t>::value)
        {
            xdrFilter.type = stellar::BINARY_FUSE_FILTER_16_BIT;
        }
        else if constexpr (std::is_same<T, uint32_t>::value)
        {
            xdrFilter.type = stellar::BINARY_FUSE_FILTER_32_BIT;
        }
        else
        {
            static_assert(!sizeof(T), "Invalid BinaryFuseFilter type");
        }

        std::copy(Seed.begin(), Seed.end(), xdrFilter.filterSeed.seed.begin());
        xdrFilter.segmentLength = SegmentLength;
        xdrFilter.segementLengthMask = SegmentLengthMask;
        xdrFilter.segmentCount = SegmentCount;
        xdrFilter.segmentCountLength = SegmentCountLength;
        xdrFilter.fingerprintLength = ArrayLength;

        // We need to convert the in-memory vector<T> into a vector<uint8_t>
        xdrFilter.fingerprints.reserve(ArrayLength * sizeof(T));
        for (T f : Fingerprints)
        {
            for (size_t byte_i = 0; byte_i < sizeof(T); ++byte_i)
            {
                xdrFilter.fingerprints.push_back((f >> (byte_i * 8)) & 0xFF);
            }
        }
    }

    bool
    operator==(binary_fuse_t const& other) const
    {
        return SegmentLength == other.SegmentLength &&
               SegmentLengthMask == other.SegmentLengthMask &&
               SegmentCount == other.SegmentCount &&
               SegmentCountLength == other.SegmentCountLength &&
               ArrayLength == other.ArrayLength && Seed == other.Seed &&
               Fingerprints == other.Fingerprints;
    }
};

template <typename T> struct binary_fuse_create_return
{
    using type = T;
};

template <typename T>
typename binary_fuse_create_return<T>::type
binary_fuse_create_from_serialized_xdr(
    stellar::SerializedBinaryFuseFilter const& xdrFilter)
{
    if constexpr (std::is_same<T, uint8_t>::value)
    {
        return binary_fuse_t<uint8_t>(xdrFilter);
    }
    else if constexpr (std::is_same<T, uint16_t>::value)
    {
        return binary_fuse_t<uint16_t>(xdrFilter);
    }
    else if constexpr (std::is_same<T, uint32_t>::value)
    {
        return binary_fuse_t<uint32_t>(xdrFilter);
    }
    else
    {
        static_assert(!sizeof(T), "Invalid BinaryFuseFilter type");
    }
}

#endif