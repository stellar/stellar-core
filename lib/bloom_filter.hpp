/*
 *********************************************************************
 *                                                                   *
 *                           Open Bloom Filter                       *
 *                                                                   *
 * Author: Arash Partow - 2000                                       *
 * URL: http://www.partow.net                                        *
 * URL: http://www.partow.net/programming/hashfunctions/index.html   *
 *                                                                   *
 * Copyright notice:                                                 *
 * Free use of the Open Bloom Filter Library is permitted under the  *
 * guidelines and in accordance with the MIT License.                *
 * http://www.opensource.org/licenses/MIT                            *
 *                                                                   *
 *********************************************************************
 */

#ifndef INCLUDE_BLOOM_FILTER_HPP
#define INCLUDE_BLOOM_FILTER_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "util/siphash.h"
#include <Tracy.hpp>
#include <cereal/archives/binary.hpp>
#include <cereal/types/array.hpp>
#include <cereal/types/vector.hpp>

static const std::size_t bits_per_char = 0x08; // 8 bits in 1 char(unsigned)

static const unsigned char bit_mask[bits_per_char] = {
    0x01, // 00000001
    0x02, // 00000010
    0x04, // 00000100
    0x08, // 00001000
    0x10, // 00010000
    0x20, // 00100000
    0x40, // 01000000
    0x80  // 10000000
};

class bloom_parameters
{
  public:
    typedef std::array<unsigned char, crypto_shorthash_KEYBYTES> rand_t;

    bloom_parameters()
        : minimum_size(1)
        , maximum_size(std::numeric_limits<unsigned long long int>::max())
        , minimum_number_of_hashes(1)
        , maximum_number_of_hashes(std::numeric_limits<unsigned int>::max())
        , projected_element_count(10000)
        , false_positive_probability(1.0 / projected_element_count)
        , random_seed()
    {
    }

    virtual ~bloom_parameters()
    {
    }

    inline bool
    operator!()
    {
        return (minimum_size > maximum_size) ||
               (minimum_number_of_hashes > maximum_number_of_hashes) ||
               (minimum_number_of_hashes < 1) ||
               (0 == maximum_number_of_hashes) ||
               (0 == projected_element_count) ||
               (false_positive_probability < 0.0) ||
               (std::numeric_limits<double>::infinity() ==
                std::abs(false_positive_probability)) ||
               (random_seed == rand_t{});
    }

    // Allowable min/max size of the bloom filter in bits
    unsigned long long int minimum_size;
    unsigned long long int maximum_size;

    // Allowable min/max number of hash functions
    unsigned int minimum_number_of_hashes;
    unsigned int maximum_number_of_hashes;

    // The approximate number of elements to be inserted
    // into the bloom filter, should be within one order
    // of magnitude. The default is 10000.
    unsigned long long int projected_element_count;

    // The approximate false positive probability expected
    // from the bloom filter. The default is assumed to be
    // the reciprocal of the projected_element_count.
    double false_positive_probability;

    rand_t random_seed;

    struct optimal_parameters_t
    {
        optimal_parameters_t() : number_of_hashes(0), table_size(0)
        {
        }

        unsigned int number_of_hashes;
        unsigned long long int table_size;
    };

    optimal_parameters_t optimal_parameters;

    virtual bool
    compute_optimal_parameters()
    {
        /*
          Note:
          The following will attempt to find the number of hash functions
          and minimum amount of storage bits required to construct a bloom
          filter consistent with the user defined false positive probability
          and estimated element insertion count.
        */

        if (!(*this))
            return false;

        double min_m = std::numeric_limits<double>::infinity();
        double min_k = 0.0;
        double k = 1.0;

        while (k < 1000.0)
        {
            const double numerator = (-k * projected_element_count);
            const double denominator =
                std::log(1.0 - std::pow(false_positive_probability, 1.0 / k));

            const double curr_m = numerator / denominator;

            if (curr_m < min_m)
            {
                min_m = curr_m;
                min_k = k;
            }

            k += 1.0;
        }

        optimal_parameters_t& optp = optimal_parameters;

        optp.number_of_hashes = static_cast<unsigned int>(min_k);

        optp.table_size = static_cast<unsigned long long int>(min_m);

        optp.table_size +=
            (((optp.table_size % bits_per_char) != 0)
                 ? (bits_per_char - (optp.table_size % bits_per_char))
                 : 0);

        if (optp.number_of_hashes < minimum_number_of_hashes)
            optp.number_of_hashes = minimum_number_of_hashes;
        else if (optp.number_of_hashes > maximum_number_of_hashes)
            optp.number_of_hashes = maximum_number_of_hashes;

        if (optp.table_size < minimum_size)
            optp.table_size = minimum_size;
        else if (optp.table_size > maximum_size)
            optp.table_size = maximum_size;

        return true;
    }
};

class bloom_filter
{
  protected:
    typedef unsigned int bloom_type;
    typedef unsigned char cell_type;
    typedef std::vector<unsigned char> table_type;

  public:
    bloom_filter()
        : hash_count_(0)
        , table_size_(0)
        , projected_element_count_(0)
        , inserted_element_count_(0)
        , random_seed_()
        , desired_false_positive_probability_(0.0)
    {
    }

    bloom_filter(const bloom_parameters& p)
        : projected_element_count_(p.projected_element_count)
        , inserted_element_count_(0)
        , random_seed_(p.random_seed)
        , desired_false_positive_probability_(p.false_positive_probability)
    {
        hash_count_ = p.optimal_parameters.number_of_hashes;
        table_size_ = p.optimal_parameters.table_size;

        bit_table_.resize(table_size_ / bits_per_char,
                          static_cast<unsigned char>(0x00));
    }

    bloom_filter(const bloom_filter& filter)
    {
        this->operator=(filter);
    }

    inline bool
    operator==(const bloom_filter& f) const
    {
        if (this != &f)
        {
            return (hash_count_ == f.hash_count_) &&
                   (table_size_ == f.table_size_) &&
                   (bit_table_.size() == f.bit_table_.size()) &&
                   (projected_element_count_ == f.projected_element_count_) &&
                   (inserted_element_count_ == f.inserted_element_count_) &&
                   (random_seed_ == f.random_seed_) &&
                   (desired_false_positive_probability_ ==
                    f.desired_false_positive_probability_) &&
                   (bit_table_ == f.bit_table_);
        }
        else
            return true;
    }

    inline bool
    operator!=(const bloom_filter& f) const
    {
        return !operator==(f);
    }

    inline bloom_filter&
    operator=(const bloom_filter& f)
    {
        if (this != &f)
        {
            hash_count_ = f.hash_count_;
            table_size_ = f.table_size_;
            bit_table_ = f.bit_table_;

            projected_element_count_ = f.projected_element_count_;
            inserted_element_count_ = f.inserted_element_count_;

            random_seed_ = f.random_seed_;

            desired_false_positive_probability_ =
                f.desired_false_positive_probability_;
        }

        return *this;
    }

    virtual ~bloom_filter()
    {
    }

    inline bool
    operator!() const
    {
        return (0 == table_size_);
    }

    inline void
    clear()
    {
        std::fill(bit_table_.begin(), bit_table_.end(),
                  static_cast<unsigned char>(0x00));
        inserted_element_count_ = 0;
    }

    inline void
    insert(const unsigned char* key_begin, const std::size_t& length)
    {
        std::size_t bit_index = 0;
        std::size_t bit = 0;

        for (auto i = 0u; i < hash_count_; ++i)
        {
            compute_indices(hash_ap(key_begin, length, i), bit_index, bit);

            bit_table_[bit_index / bits_per_char] |= bit_mask[bit];
        }

        ++inserted_element_count_;
    }

    template <typename T>
    inline void
    insert(const T& t)
    {
        // Note: T must be a C++ POD type.
        insert(reinterpret_cast<const unsigned char*>(&t), sizeof(T));
    }

    inline void
    insert(const std::string& key)
    {
        insert(reinterpret_cast<const unsigned char*>(key.data()), key.size());
    }

    inline void
    insert(const char* data, const std::size_t& length)
    {
        insert(reinterpret_cast<const unsigned char*>(data), length);
    }

    template <typename InputIterator>
    inline void
    insert(const InputIterator begin, const InputIterator end)
    {
        InputIterator itr = begin;

        while (end != itr)
        {
            insert(*(itr++));
        }
    }

    inline virtual bool
    contains(const unsigned char* key_begin, const std::size_t length) const
    {
        ZoneScoped;
        std::size_t bit_index = 0;
        std::size_t bit = 0;

        for (auto i = 0u; i < hash_count_; ++i)
        {
            compute_indices(hash_ap(key_begin, length, i), bit_index, bit);

            if ((bit_table_[bit_index / bits_per_char] & bit_mask[bit]) !=
                bit_mask[bit])
            {
                return false;
            }
        }

        return true;
    }

    template <typename T>
    inline bool
    contains(const T& t) const
    {
        return contains(reinterpret_cast<const unsigned char*>(&t),
                        static_cast<std::size_t>(sizeof(T)));
    }

    inline bool
    contains(const std::string& key) const
    {
        return contains(reinterpret_cast<const unsigned char*>(key.c_str()),
                        key.size());
    }

    inline bool
    contains(const char* data, const std::size_t& length) const
    {
        return contains(reinterpret_cast<const unsigned char*>(data), length);
    }

    template <typename InputIterator>
    inline InputIterator
    contains_all(const InputIterator begin, const InputIterator end) const
    {
        InputIterator itr = begin;

        while (end != itr)
        {
            if (!contains(*itr))
            {
                return itr;
            }

            ++itr;
        }

        return end;
    }

    template <typename InputIterator>
    inline InputIterator
    contains_none(const InputIterator begin, const InputIterator end) const
    {
        InputIterator itr = begin;

        while (end != itr)
        {
            if (contains(*itr))
            {
                return itr;
            }

            ++itr;
        }

        return end;
    }

    inline virtual unsigned long long int
    size() const
    {
        return table_size_;
    }

    inline unsigned long long int
    element_count() const
    {
        return inserted_element_count_;
    }

    inline double
    effective_fpp() const
    {
        /*
          Note:
          The effective false positive probability is calculated using the
          designated table size and hash function count in conjunction with
          the current number of inserted elements - not the user defined
          predicated/expected number of inserted elements.
        */
        return std::pow(1.0 - std::exp(-1.0 * hash_count_ *
                                       inserted_element_count_ / size()),
                        1.0 * hash_count_);
    }

    inline bloom_filter&
    operator&=(const bloom_filter& f)
    {
        /* intersection */
        if ((hash_count_ == f.hash_count_) && (table_size_ == f.table_size_) &&
            (random_seed_ == f.random_seed_))
        {
            for (std::size_t i = 0; i < bit_table_.size(); ++i)
            {
                bit_table_[i] &= f.bit_table_[i];
            }
        }

        return *this;
    }

    inline bloom_filter&
    operator|=(const bloom_filter& f)
    {
        /* union */
        if ((hash_count_ == f.hash_count_) && (table_size_ == f.table_size_) &&
            (random_seed_ == f.random_seed_))
        {
            for (std::size_t i = 0; i < bit_table_.size(); ++i)
            {
                bit_table_[i] |= f.bit_table_[i];
            }
        }

        return *this;
    }

    inline bloom_filter&
    operator^=(const bloom_filter& f)
    {
        /* difference */
        if ((hash_count_ == f.hash_count_) && (table_size_ == f.table_size_) &&
            (random_seed_ == f.random_seed_))
        {
            for (std::size_t i = 0; i < bit_table_.size(); ++i)
            {
                bit_table_[i] ^= f.bit_table_[i];
            }
        }

        return *this;
    }

    inline const cell_type*
    table() const
    {
        return bit_table_.data();
    }

    inline std::size_t
    hash_count()
    {
        return hash_count_;
    }

    template <class Archive>
    void
    serialize(Archive& ar)
    {
        ar(bit_table_, hash_count_, table_size_, projected_element_count_,
           inserted_element_count_, random_seed_,
           desired_false_positive_probability_);
    }

  protected:
    inline virtual void
    compute_indices(const bloom_type& hash, std::size_t& bit_index,
                    std::size_t& bit) const
    {
        bit_index = hash % table_size_;
        bit = bit_index % bits_per_char;
    }

    inline bloom_type
    hash_ap(const unsigned char* begin, std::size_t remaining_length,
            bloom_type hash_num) const
    {
        SipHash24 sh(random_seed_.data());
        sh.update(reinterpret_cast<uint8_t*>(&hash_num), sizeof(hash_num));
        sh.update(begin, remaining_length);
        return static_cast<bloom_type>(sh.digest() &
                                       std::numeric_limits<bloom_type>::max());
    }

    std::vector<unsigned char> bit_table_;
    unsigned int hash_count_;
    unsigned long long int table_size_;
    unsigned long long int projected_element_count_;
    unsigned long long int inserted_element_count_;
    bloom_parameters::rand_t random_seed_;
    double desired_false_positive_probability_;
};

inline bloom_filter
operator&(const bloom_filter& a, const bloom_filter& b)
{
    bloom_filter result = a;
    result &= b;
    return result;
}

inline bloom_filter
operator|(const bloom_filter& a, const bloom_filter& b)
{
    bloom_filter result = a;
    result |= b;
    return result;
}

inline bloom_filter
operator^(const bloom_filter& a, const bloom_filter& b)
{
    bloom_filter result = a;
    result ^= b;
    return result;
}

class compressible_bloom_filter : public bloom_filter
{
  public:
    compressible_bloom_filter(const bloom_parameters& p) : bloom_filter(p)
    {
        size_list.push_back(table_size_);
    }

    inline unsigned long long int
    size() const
    {
        return size_list.back();
    }

    inline bool
    compress(const double& percentage)
    {
        if ((percentage < 0.0) || (percentage >= 100.0))
        {
            return false;
        }

        unsigned long long int original_table_size = size_list.back();
        unsigned long long int new_table_size =
            static_cast<unsigned long long int>(
                (size_list.back() * (1.0 - (percentage / 100.0))));

        new_table_size -= new_table_size % bits_per_char;

        if ((bits_per_char > new_table_size) ||
            (new_table_size >= original_table_size))
        {
            return false;
        }

        desired_false_positive_probability_ = effective_fpp();

        const unsigned long long int new_tbl_raw_size =
            new_table_size / bits_per_char;

        table_type tmp(new_tbl_raw_size);

        std::copy(bit_table_.begin(), bit_table_.begin() + new_tbl_raw_size,
                  tmp.begin());

        typedef table_type::iterator itr_t;

        itr_t itr = bit_table_.begin() + (new_table_size / bits_per_char);
        itr_t end = bit_table_.begin() + (original_table_size / bits_per_char);
        itr_t itr_tmp = tmp.begin();

        while (end != itr)
        {
            *(itr_tmp++) |= (*itr++);
        }

        std::swap(bit_table_, tmp);

        size_list.push_back(new_table_size);

        return true;
    }

  private:
    inline void
    compute_indices(const bloom_type& hash, std::size_t& bit_index,
                    std::size_t& bit) const
    {
        bit_index = hash;

        for (std::size_t i = 0; i < size_list.size(); ++i)
        {
            bit_index %= size_list[i];
        }

        bit = bit_index % bits_per_char;
    }

    std::vector<unsigned long long int> size_list;
};

#endif

/*
  Note 1:
  If it can be guaranteed that bits_per_char will be of the form 2^n then
  the following optimization can be used:

  bit_table_[bit_index >> n] |= bit_mask[bit_index & (bits_per_char - 1)];

  Note 2:
  For performance reasons where possible when allocating memory it should
  be aligned (aligned_alloc) according to the architecture being used.
*/