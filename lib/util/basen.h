#pragma once

/**
* base-n, 1.0
* Copyright (C) 2012 Andrzej Zawadzki (azawadzki@gmail.com)
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
**/
#ifndef BASEN_HPP
#define BASEN_HPP

#include <algorithm>
#include <cctype>
#include <cassert>
#include <cstring>

namespace bn
{

template<class Iter1, class Iter2>
void encode_b16(Iter1 start, Iter1 end, Iter2 out);

template<class Iter1, class Iter2>
void encode_b32(Iter1 start, Iter1 end, Iter2 out);

template<class Iter1, class Iter2>
void encode_b64(Iter1 start, Iter1 end, Iter2 out);

template<class Iter1, class Iter2>
void decode_b16(Iter1 start, Iter1 end, Iter2 out);

template<class Iter1, class Iter2>
void decode_b32(Iter1 start, Iter1 end, Iter2 out);

template<class Iter1, class Iter2>
void decode_b64(Iter1 start, Iter1 end, Iter2 out);

namespace impl
{

char extract_partial_bits(char value, unsigned int start_bit, unsigned int bits_count);
char extract_overlapping_bits(char previous, char next, unsigned int start_bit, unsigned int bits_count);

struct b16_conversion_traits
{
    static int group_length()
    {
        return 4;
    }

    static char encode(unsigned int index)
    {
        static const char dictionary[17] = "0123456789ABCDEF";
        assert(index < 16);
        return dictionary[index];
    }

    static char decode(char c)
    {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        else if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }
};

struct b32_conversion_traits
{
    static int group_length()
    {
        return 5;
    }

    static char encode(unsigned int index)
    {
        static const char dictionary[33] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        assert(index < 32);
        return dictionary[index];
    }

    static char decode(char c)
    {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        else if (c >= '2' && c <= '7') {
            return c - '2' + 26;
        }
        return -1;
    }
};

struct b64_conversion_traits
{
    static int group_length()
    {
        return 6;
    }

    static char encode(unsigned int index)
    {
        static const char dictionary[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        assert(index < 64);
        return dictionary[index];
    }

    static char decode(char c)
    {
        const int indexOf_a = 26;
        const int indexOf_0 = indexOf_a+ indexOf_a;
        const int indexOf_Plus = indexOf_0 + 10;
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        else if (c >= 'a' && c <= 'z') {
            return c - 'a' + indexOf_a;
        }
        else if (c >= '0' && c <= '9') {
            return c - '0' + indexOf_0;
        }
        else if (c == '+') {
            return c - '+' + indexOf_Plus;
        }
        else if (c == '/') {
            return c - '/' + indexOf_Plus + 1;
        }
        return -1;
    }
};

template<class ConversionTraits, class Iter1, class Iter2>
void decode(Iter1 start, Iter1 end, Iter2 out)
{
    Iter1 iter = start;
    int output_current_bit = 0;
    char buffer = 0;
    int trailing_equal = 0;

    while (iter != end) {
        if (*iter == '=') {
            trailing_equal++;
            iter++;
            continue;
        }
        if (std::isspace(*iter)) {
            ++iter;
            continue;
        }
        char value = ConversionTraits::decode(*iter);
        if (value == -1) {
            // malformed data, but let's go on...
            ++iter;
            continue;
        }
        int bits_in_current_byte = std::min<int>(output_current_bit + ConversionTraits::group_length(), 8) - output_current_bit;
        if (bits_in_current_byte == ConversionTraits::group_length()) {
            // the value fits within current byte, so we can extract it directly
            buffer |= (value << (8 - output_current_bit - ConversionTraits::group_length())) & 0xFF;
            output_current_bit += ConversionTraits::group_length();
            // check if we filled up current byte completely; in such case we flush output and continue
            if (output_current_bit == 8) {
                *out++ = buffer;
                buffer = 0;
                output_current_bit = 0;
            }
        }
        else {
            // the value span across current and next byte
            int bits_in_next_byte = ConversionTraits::group_length() - bits_in_current_byte;
            // fill current byte and flush it to our output
            buffer |= value >> bits_in_next_byte;
            *out++ = buffer;
            buffer = 0;
            // save the remainder of our value in the buffer; it will be flushed
            // during next iterations
            buffer |= (value << (8 - bits_in_next_byte)) & 0xFF;
            output_current_bit = bits_in_next_byte;
        }
        ++iter;
    }
    if (output_current_bit != 0)
    {
        // see if we have truncated data (if so, just include it)
        int trailing_bits = trailing_equal*ConversionTraits::group_length() + output_current_bit;
        if ((trailing_bits & 7) !=0)
        {
            *out++ = buffer;
        }
    }
}

template<class ConversionTraits, class Iter1, class Iter2>
void encode(Iter1 start, Iter1 end, Iter2 out)
{
    Iter1 iter = start;
    int start_bit = 0;
    bool has_backlog = false;
    char backlog = 0;

    while (has_backlog || iter != end) {
        if (!has_backlog) {
            if (start_bit + ConversionTraits::group_length() < 8) {
                // the value fits within single byte, so we can extract it
                // directly
                char v = extract_partial_bits(*iter, start_bit, ConversionTraits::group_length());
                *out++ = ConversionTraits::encode(v);
                // since we know that start_bit + ConversionTraits::group_length() < 8 we don't need to go
                // to the next byte
                start_bit += ConversionTraits::group_length();
            }
            else {
                // our bits are spanning across byte border; we need to keep the
                // starting point and move over to next byte.
                backlog = *iter++;
                has_backlog = true;
            }
        }
        else {
            // encode value which is made from bits spanning across byte
            // boundary
            char v = extract_overlapping_bits(backlog, (iter != end) ? *iter : 0, start_bit, ConversionTraits::group_length());
            *out++ = ConversionTraits::encode(v);
            has_backlog = false;
            start_bit = (start_bit + ConversionTraits::group_length()) % 8;
        }
    }
    while (start_bit != 0)
    {
        *out++ = '=';
        start_bit = (start_bit + ConversionTraits::group_length()) % 8;
    }
}

inline char extract_partial_bits(char value, unsigned int start_bit, unsigned int bits_count)
{
    assert(start_bit + bits_count < 8);
    char t1 = value >> (8 - bits_count - start_bit);
    char t2 = t1 & ~(-1 << bits_count);
    return t2;
}

inline char extract_overlapping_bits(char previous, char next, unsigned int start_bit, unsigned int bits_count)
{
    assert(start_bit + bits_count < 16);
    int bits_count_in_previous = 8 - start_bit;
    int bits_count_in_next = bits_count - bits_count_in_previous;
    char t1 = (previous << bits_count_in_next) & 0xFF;
    char t2 = next >> (8 - bits_count_in_next) & ~(-1 << bits_count_in_next);
    return (t1 | t2) & ~(-1 << bits_count);
}

} // impl

using namespace bn::impl;

inline size_t encoded_size16(size_t rawsize)
{
    return (rawsize * 2);
}

inline size_t encoded_size32(size_t rawsize)
{
    return ((rawsize + 4) / 5 * 8);
}

inline size_t encoded_size64(size_t rawsize)
{
    return ((rawsize + 2) / 3 * 4);
}

template<class Iter1, class Iter2> inline
void encode_b16(Iter1 start, Iter1 end, Iter2 out)
{
    encode<b16_conversion_traits>(start, end, out);
}

template<class Iter1, class Iter2> inline
void encode_b32(Iter1 start, Iter1 end, Iter2 out)
{
    encode<b32_conversion_traits>(start, end, out);
}

template<class Iter1, class Iter2> inline
void encode_b64(Iter1 start, Iter1 end, Iter2 out)
{
    encode<b64_conversion_traits>(start, end, out);
}

template<class T> inline
std::string encode_b16(T const& v)
{
    std::string res;
    res.reserve(encoded_size16(v.size()*sizeof(typename T::value_type)) + 1);
    encode_b16(v.begin(), v.end(), std::back_inserter(res));
    return res;
}

template<class T> inline
std::string encode_b32(T const& v)
{
    std::string res;
    res.reserve(encoded_size64(v.size()*sizeof(typename T::value_type)) + 1);
    encode_b32(v.begin(), v.end(), std::back_inserter(res));
    return res;
}

template<class T> inline
std::string encode_b64(T const& v)
{
    std::string res;
    res.reserve(encoded_size64(v.size()*sizeof(typename T::value_type)) + 1);
    encode_b64(v.begin(), v.end(), std::back_inserter(res));
    return res;
}

template<class Iter1, class Iter2> inline
void decode_b16(Iter1 start, Iter1 end, Iter2 out)
{
    decode<b16_conversion_traits>(start, end, out);
}

template<class Iter1, class Iter2> inline
void decode_b32(Iter1 start, Iter1 end, Iter2 out)
{
    decode<b32_conversion_traits>(start, end, out);
}

template<class Iter1, class Iter2> inline
void decode_b64(Iter1 start, Iter1 end, Iter2 out)
{
    decode<b64_conversion_traits>(start, end, out);
}

template<class V, class T> inline
void decode_b16(V const& v, T& out)
{
    out.clear();
    out.reserve(v.size()*sizeof(typename T::value_type));
    decode_b16(v.begin(), v.end(), std::back_inserter(out));
}

template<class V, class T> inline
void decode_b32(V const& v, T& out)
{
    out.clear();
    out.reserve(v.size()*sizeof(typename T::value_type));
    decode_b32(v.begin(), v.end(), std::back_inserter(out));
}

template<class V, class T> inline
void decode_b64(V const& v, T& out)
{
    out.clear();
    out.reserve(v.size()*sizeof(typename T::value_type));
    decode_b64(v.begin(), v.end(), std::back_inserter(out));
}

} // bn

#endif // BASEN_HPP
