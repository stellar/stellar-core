#ifndef BINARYFUSEFILTER_H
#define BINARYFUSEFILTER_H
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdexcept>
#include <stdint.h>

#include <algorithm>
#include <limits>
#include <type_traits>
#include <vector>
#ifndef XOR_MAX_ITERATIONS
#define XOR_MAX_ITERATIONS \
    100 // probability of success should always be > 0.5 so 100 iterations is
        // highly unlikely
#endif

/**
 * We start with a few utilities.
 ***/
static inline uint64_t
binary_fuse_murmur64(uint64_t h)
{
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    return h;
}
static inline uint64_t
binary_fuse_mix_split(uint64_t key, uint64_t seed)
{
    return binary_fuse_murmur64(key + seed);
}
static inline uint64_t
binary_fuse_rotl64(uint64_t n, unsigned int c)
{
    return (n << (c & 63)) | (n >> ((-c) & 63));
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

template <typename T,
          class = typename std::enable_if_t<std::is_unsigned<T>::value>>
class binary_fuse_t
{
  private:
    uint64_t _seed;
    uint32_t _segmentLength;
    uint32_t _segmentLengthMask;
    uint32_t _segmentCount;
    uint32_t _segmentCountLength;
    uint32_t _arrayLength;
    std::vector<T> _fingerprints;

    struct binary_hashes_t
    {
        uint32_t h0;
        uint32_t h1;
        uint32_t h2;
    };

    binary_hashes_t
    hash_batch(uint64_t hash) const
    {
        uint64_t hi = binary_fuse_mulhi(hash, _segmentCountLength);
        binary_hashes_t ans;
        ans.h0 = (uint32_t)hi;
        ans.h1 = ans.h0 + _segmentLength;
        ans.h2 = ans.h1 + _segmentLength;
        ans.h1 ^= (uint32_t)(hash >> 18) & _segmentLengthMask;
        ans.h2 ^= (uint32_t)(hash)&_segmentLengthMask;
        return ans;
    }

    uint32_t
    binary_fuse_hash(int index, uint64_t hash) const
    {
        uint64_t h = binary_fuse_mulhi(hash, _segmentCountLength);
        h += index * _segmentLength;
        // keep the lower 36 bits
        uint64_t hh = hash & ((1UL << 36) - 1);
        // index 0: right shift by 36; index 1: right shift by 18; index 2: no
        // shift
        h ^= (size_t)((hh >> (36 - 18 * index)) & _segmentLengthMask);
        return h;
    }

  public:
    // allocate enough capacity for a set containing up to 'size' elements
    // size should be at least 2.
    explicit binary_fuse_t(uint32_t size)
    {
        if (size < 2)
        {
            throw std::runtime_error("size should be at least 2");
        }

        uint32_t arity = 3;
        _segmentLength = binary_fuse_calculate_segment_length(arity, size);
        if (_segmentLength > 262144)
        {
            _segmentLength = 262144;
        }
        _segmentLengthMask = _segmentLength - 1;
        double sizeFactor = binary_fuse_calculate_size_factor(arity, size);
        uint32_t capacity = (uint32_t)(round((double)size * sizeFactor));
        uint32_t initSegmentCount =
            (capacity + _segmentLength - 1) / _segmentLength - (arity - 1);
        _arrayLength = (initSegmentCount + arity - 1) * _segmentLength;
        _segmentCount = (_arrayLength + _segmentLength - 1) / _segmentLength;
        if (_segmentCount <= arity - 1)
        {
            _segmentCount = 1;
        }
        else
        {
            _segmentCount = _segmentCount - (arity - 1);
        }
        _arrayLength = (_segmentCount + arity - 1) * _segmentLength;
        _segmentCountLength = _segmentCount * _segmentLength;
        _fingerprints.resize(_arrayLength);
    }

    // Report if the key is in the set, with false positive rate.
    bool
    contain(uint64_t key) const
    {
        uint64_t hash = binary_fuse_mix_split(key, _seed);
        T f = binary_fuse_fingerprint(hash);
        binary_hashes_t hashes = hash_batch(hash);
        f ^= _fingerprints[hashes.h0] ^ _fingerprints[hashes.h1] ^
             _fingerprints[hashes.h2];
        return f == 0;
    }

    // report memory usage
    size_t
    size_in_bytes() const
    {
        return _arrayLength * sizeof(T) + sizeof(*this);
    }

    // Construct the filter, returns true on success, false on failure.
    // The algorithm fails when there is insufficient memory.
    // For best performance, the caller should ensure that there are not
    // too many duplicated keys.
    // While highly improbable, it is possible that the population fails, at
    // which point the seed must be rotated. keys will be sorted and duplicates
    // removed if any duplicate keys exist
    [[nodiscard]] bool
    populate(std::vector<uint64_t>& keys)
    {
        if (keys.size() > std::numeric_limits<uint32_t>::max())
        {
            throw std::runtime_error("size should be at most 2^32");
        }

        uint32_t size = keys.size();

        uint64_t rng_counter = 0x726b2b9d438b9d4d;
        _seed = binary_fuse_rng_splitmix64(&rng_counter);

        std::vector<uint64_t> reverseOrder(size + 1);
        uint32_t capacity = _arrayLength;

        std::vector<uint32_t> alone(capacity);
        std::vector<uint8_t> t2count(capacity);
        std::vector<uint8_t> reverseH(size);
        std::vector<uint64_t> t2hash(capacity);

        uint32_t blockBits = 1;
        while (((uint32_t)1 << blockBits) < _segmentCount)
        {
            blockBits += 1;
        }
        uint32_t block = ((uint32_t)1 << blockBits);

        std::vector<uint32_t> startPos(1 << blockBits);
        uint32_t h012[5];

        reverseOrder[size] = 1;
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
                startPos[i] = ((uint64_t)i * size) >> blockBits;
            }

            uint64_t maskblock = block - 1;
            for (uint32_t i = 0; i < size; i++)
            {
                uint64_t hash = binary_fuse_murmur64(keys[i] + _seed);
                uint64_t segment_index = hash >> (64 - blockBits);
                while (reverseOrder[startPos[segment_index]] != 0)
                {
                    segment_index++;
                    segment_index &= maskblock;
                }
                reverseOrder[startPos[segment_index]] = hash;
                startPos[segment_index]++;
            }
            int error = 0;
            uint32_t duplicates = 0;
            for (uint32_t i = 0; i < size; i++)
            {
                uint64_t hash = reverseOrder[i];
                uint32_t h0 = binary_fuse_hash(0, hash);
                t2count[h0] += 4;
                t2hash[h0] ^= hash;
                uint32_t h1 = binary_fuse_hash(1, hash);
                t2count[h1] += 4;
                t2count[h1] ^= 1;
                t2hash[h1] ^= hash;
                uint32_t h2 = binary_fuse_hash(2, hash);
                t2count[h2] += 4;
                t2hash[h2] ^= hash;
                t2count[h2] ^= 2;
                if ((t2hash[h0] & t2hash[h1] & t2hash[h2]) == 0)
                {
                    if (((t2hash[h0] == 0) && (t2count[h0] == 8)) ||
                        ((t2hash[h1] == 0) && (t2count[h1] == 8)) ||
                        ((t2hash[h2] == 0) && (t2count[h2] == 8)))
                    {
                        duplicates += 1;
                        t2count[h0] -= 4;
                        t2hash[h0] ^= hash;
                        t2count[h1] -= 4;
                        t2count[h1] ^= 1;
                        t2hash[h1] ^= hash;
                        t2count[h2] -= 4;
                        t2count[h2] ^= 2;
                        t2hash[h2] ^= hash;
                    }
                }
                error = (t2count[h0] < 4) ? 1 : error;
                error = (t2count[h1] < 4) ? 1 : error;
                error = (t2count[h2] < 4) ? 1 : error;
            }
            if (error)
            {
                std::fill(reverseOrder.begin(), reverseOrder.end(), 0);
                std::fill(t2count.begin(), t2count.end(), 0);
                std::fill(t2hash.begin(), t2hash.end(), 0);

                // TOOD: Actual random
                _seed = binary_fuse_rng_splitmix64(&rng_counter);
                continue;
            }

            // End of key addition
            uint32_t Qsize = 0;
            // Add sets with one key to the queue.
            for (uint32_t i = 0; i < capacity; i++)
            {
                alone[Qsize] = i;
                Qsize += ((t2count[i] >> 2) == 1) ? 1 : 0;
            }
            uint32_t stacksize = 0;
            while (Qsize > 0)
            {
                Qsize--;
                uint32_t index = alone[Qsize];
                if ((t2count[index] >> 2) == 1)
                {
                    uint64_t hash = t2hash[index];

                    // h012[0] = binary_fuse16_hash(0, hash);
                    h012[1] = binary_fuse_hash(1, hash);
                    h012[2] = binary_fuse_hash(2, hash);
                    h012[3] = binary_fuse_hash(0, hash); // == h012[0];
                    h012[4] = h012[1];
                    uint8_t found = t2count[index] & 3;
                    reverseH[stacksize] = found;
                    reverseOrder[stacksize] = hash;
                    stacksize++;
                    uint32_t other_index1 = h012[found + 1];
                    alone[Qsize] = other_index1;
                    Qsize += ((t2count[other_index1] >> 2) == 2 ? 1 : 0);

                    t2count[other_index1] -= 4;
                    t2count[other_index1] ^= binary_fuse_mod3(found + 1);
                    t2hash[other_index1] ^= hash;

                    uint32_t other_index2 = h012[found + 2];
                    alone[Qsize] = other_index2;
                    Qsize += ((t2count[other_index2] >> 2) == 2 ? 1 : 0);
                    t2count[other_index2] -= 4;
                    t2count[other_index2] ^= binary_fuse_mod3(found + 2);
                    t2hash[other_index2] ^= hash;
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
            _seed = binary_fuse_rng_splitmix64(&rng_counter);
        }

        for (uint32_t i = size - 1; i < size; i--)
        {
            // the hash of the key we insert next
            uint64_t hash = reverseOrder[i];
            T xor2 = binary_fuse_fingerprint(hash);
            uint8_t found = reverseH[i];
            h012[0] = binary_fuse_hash(0, hash);
            h012[1] = binary_fuse_hash(1, hash);
            h012[2] = binary_fuse_hash(2, hash);
            h012[3] = h012[0];
            h012[4] = h012[1];
            _fingerprints[h012[found]] = xor2 ^ _fingerprints[h012[found + 1]] ^
                                         _fingerprints[h012[found + 2]];
        }
        return true;
    }
};

// False postive rate: 1/256
// Approximate bits per entry: 9
typedef binary_fuse_t<uint8_t> binary_fuse8_t;

// False postive rate: 1/65536
// Approximate bits per entry: 18
typedef binary_fuse_t<uint16_t> binary_fuse16_t;

// False postive rate: 1 / 4 billion
// Approximate bits per entry: 36
typedef binary_fuse_t<uint32_t> binary_fuse32_t;

#endif