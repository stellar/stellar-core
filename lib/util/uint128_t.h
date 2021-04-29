/*
uint128_t.h
An unsigned 128 bit integer type for C++

Copyright (c) 2013 - 2017 Jason Lee @ calccrypto at gmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

With much help from Auston Sterling

Thanks to Stefan Deigmüller for finding
a bug in operator*.

Thanks to François Dessenne for convincing me
to do a general rewrite of this class.

when defining UNSAFE_UINT128_OPS additional behaviors are enabled.

for example:
// implicit typecast Operators,
// short for (short)uint128_t(y).lower()
// or (char)uint128_t(y)
char x = uint128_t(y);

// arithmetic operators with short type as lhs,
// short for 2 * uint128_t(y).lower()
auto x = 2 * uint128_t(y);
*/

#ifndef __UINT128_T__
#define __UINT128_T__

#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__BYTE_ORDER) && __BYTE_ORDER == __BIG_ENDIAN || \
    defined(__BIG_ENDIAN__) ||                               \
    defined(__ARMEB__) ||                                    \
    defined(__THUMBEB__) ||                                  \
    defined(__AARCH64EB__) ||                                \
    defined(_MIBSEB) || defined(__MIBSEB) || defined(__MIBSEB__)
#ifndef __BIG_ENDIAN__
#define __BIG_ENDIAN__
#endif
#elif defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN || \
    defined(__LITTLE_ENDIAN__) ||                                 \
    defined(__ARMEL__) ||                                         \
    defined(__THUMBEL__) ||                                       \
    defined(__AARCH64EL__) ||                                     \
    defined(_MIPSEL) || defined(__MIPSEL) || defined(__MIPSEL__) || \
    defined(_WIN32) || defined(__i386__) || defined(__x86_64__) || \
    defined(_X86_) || defined(_IA64_)
#ifndef __LITTLE_ENDIAN__
#define __LITTLE_ENDIAN__
#endif
#else
#error "I don't know what architecture this is!"
#endif

// We're not building a shared library, skip dllimport/dllexport stuff.
#define UINT128_T_EXTERN

class UINT128_T_EXTERN uint128_t;

// Give uint128_t type traits
namespace std {  // This is probably not a good idea
    template <> struct is_arithmetic <uint128_t> : std::true_type {};
    template <> struct is_integral   <uint128_t> : std::true_type {};
    template <> struct is_unsigned   <uint128_t> : std::true_type {};
}

#ifdef UNSAFE_UINT128_OPS
#define IMPLICIT_UNSAFE_UINT128_OPS
#else
#define IMPLICIT_UNSAFE_UINT128_OPS explicit
#endif

class uint128_t{
    private:
#ifdef __BIG_ENDIAN__
        uint64_t UPPER, LOWER;
#endif
#ifdef __LITTLE_ENDIAN__
        uint64_t LOWER, UPPER;
#endif

    public:
        // Constructors
        uint128_t() = default;
        uint128_t(const uint128_t & rhs) = default;
        uint128_t(uint128_t && rhs) = default;
        uint128_t(std::string const& s);
        uint128_t(const char *s);

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value &&
                                                                 std::is_unsigned<T>::value, T>::type >
        uint128_t(const T & rhs)
#ifdef __BIG_ENDIAN__
            : UPPER(0), LOWER(rhs)
#endif
#ifdef __LITTLE_ENDIAN__
            : LOWER(rhs), UPPER(0)
#endif
        {}

        template <typename S, typename T, typename = typename std::enable_if <std::is_integral<S>::value &&
                                                                              std::is_integral<T>::value &&
                                                                              std::is_unsigned<S>::value &&
                                                                              std::is_unsigned<T>::value
                                                                              , void>::type>
        uint128_t(const S & upper_rhs, const T & lower_rhs)
#ifdef __BIG_ENDIAN__
            : UPPER(upper_rhs), LOWER(lower_rhs)
#endif
#ifdef __LITTLE_ENDIAN__
            : LOWER(lower_rhs), UPPER(upper_rhs)
#endif
        {
        }

        //  RHS input args only

        // Assignment Operator
        uint128_t & operator=(const uint128_t & rhs) = default;
        uint128_t & operator=(uint128_t && rhs) = default;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value &&
                                                                 std::is_unsigned<T>::value, T>::type >
        uint128_t & operator=(const T & rhs){
            UPPER = 0;
            LOWER = rhs;
            return *this;
        }

        // Typecast Operators
        IMPLICIT_UNSAFE_UINT128_OPS operator bool() const;
        IMPLICIT_UNSAFE_UINT128_OPS operator uint8_t() const;
        IMPLICIT_UNSAFE_UINT128_OPS operator uint16_t() const;
        IMPLICIT_UNSAFE_UINT128_OPS operator uint32_t() const;
        IMPLICIT_UNSAFE_UINT128_OPS operator uint64_t() const;

        // Bitwise Operators
        uint128_t operator&(const uint128_t & rhs) const;

        void export_bits(std::vector<uint8_t> & ret) const;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t operator&(const T & rhs) const{
            return *this & uint128_t(rhs);
        }

        uint128_t & operator&=(const uint128_t & rhs);

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t & operator&=(const T & rhs){
            return *this &= uint128_t(rhs);
        }

        uint128_t operator|(const uint128_t & rhs) const;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t operator|(const T & rhs) const{
            return *this | uint128_t(rhs);
        }

        uint128_t & operator|=(const uint128_t & rhs);

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t & operator|=(const T & rhs){
            return *this |= uint128_t(rhs);
        }

        uint128_t operator^(const uint128_t & rhs) const;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t operator^(const T & rhs) const{
            return *this ^ uint128_t(rhs);
        }

        uint128_t & operator^=(const uint128_t & rhs);

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t & operator^=(const T & rhs){
            return *this ^= uint128_t(rhs);
        }

        uint128_t operator~() const;

        // Bit Shift Operators
        uint128_t operator<<(const uint128_t & rhs) const;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t operator<<(const T & rhs) const{
            return *this << uint128_t(rhs);
        }

        uint128_t & operator<<=(const uint128_t & rhs);

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t & operator<<=(const T & rhs){
            *this = *this << uint128_t(rhs);
            return *this;
        }

        uint128_t operator>>(const uint128_t & rhs) const;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t operator>>(const T & rhs) const{
            return *this >> uint128_t(rhs);
        }

        uint128_t & operator>>=(const uint128_t & rhs);

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t & operator>>=(const T & rhs){
            *this = *this >> uint128_t(rhs);
            return *this;
        }

        // Logical Operators
        bool operator!() const;
        bool operator&&(const uint128_t & rhs) const;
        bool operator||(const uint128_t & rhs) const;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        bool operator&&(const T & rhs) const{
            return *this && uint128_t(rhs);
        }

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        bool operator||(const T & rhs) const{
            return *this || uint128_t(rhs);
        }

        // Comparison Operators
        bool operator==(const uint128_t & rhs) const;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        bool operator==(const T & rhs) const{
            return *this == uint128_t(rhs);
        }

        bool operator!=(const uint128_t & rhs) const;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        bool operator!=(const T & rhs) const{
            return *this != uint128_t(rhs);
        }

        bool operator>(const uint128_t & rhs) const;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        bool operator>(const T & rhs) const{
            return *this > uint128_t(rhs);
        }

        bool operator<(const uint128_t & rhs) const;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        bool operator<(const T & rhs) const{
            return *this < uint128_t(rhs);
        }

        bool operator>=(const uint128_t & rhs) const;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        bool operator>=(const T & rhs) const{
            return *this >= uint128_t(rhs);
        }

        bool operator<=(const uint128_t & rhs) const;

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        bool operator<=(const T & rhs) const{
            return *this <= uint128_t(rhs);
        }

        // Arithmetic Operators
        uint128_t operator+(const uint128_t & rhs) const;
        uint128_t & operator+=(const uint128_t & rhs);
        uint128_t operator-(const uint128_t & rhs) const;
        uint128_t & operator-=(const uint128_t & rhs);
        uint128_t operator*(const uint128_t & rhs) const;
        uint128_t & operator*=(const uint128_t & rhs);
        uint128_t operator/(const uint128_t & rhs) const;
        uint128_t & operator/=(const uint128_t & rhs);
        uint128_t operator%(const uint128_t & rhs) const;
        uint128_t & operator%=(const uint128_t & rhs);

#ifdef UNSAFE_UINT128_OPS
        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t operator+(const T & rhs) const{
            return *this + uint128_t(rhs);
        }

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t & operator+=(const T & rhs){
            return *this += uint128_t(rhs);
        }

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t operator-(const T & rhs) const{
            return *this - uint128_t(rhs);
        }

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t & operator-=(const T & rhs){
            return *this = *this - uint128_t(rhs);
        }


        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t operator*(const T & rhs) const{
            return *this * uint128_t(rhs);
        }

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t & operator*=(const T & rhs){
            return *this = *this * uint128_t(rhs);
        }

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t operator/(const T & rhs) const{
            return *this / uint128_t(rhs);
        }

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t & operator/=(const T & rhs){
            return *this = *this / uint128_t(rhs);
        }

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t operator%(const T & rhs) const{
            return *this % uint128_t(rhs);
        }

        template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
        uint128_t & operator%=(const T & rhs){
            return *this = *this % uint128_t(rhs);
        }
#endif

    private:
        std::pair <uint128_t, uint128_t> divmod(const uint128_t & lhs, const uint128_t & rhs) const;
        void init(const char * s);
        void ConvertToVector(std::vector<uint8_t> & current, const uint64_t & val) const;
        void _init_hex(const char *s);
        void _init_dec(const char *s);
        void _init_oct(const char *s);

    public:

        // Increment Operator
        uint128_t & operator++();
        uint128_t operator++(int);

        // Decrement Operator
        uint128_t & operator--();
        uint128_t operator--(int);

        // Nothing done since promotion doesn't work here
        uint128_t operator+() const;

        // two's complement
        uint128_t operator-() const;

        // Get private values
        const uint64_t & upper() const;
        const uint64_t & lower() const;

        // Get bitsize of value
        uint8_t bits() const;

        // Get string representation of value
        std::string str(uint8_t base = 10, const unsigned int & len = 0) const;
};

// useful values
UINT128_T_EXTERN extern const uint128_t uint128_0;
UINT128_T_EXTERN extern const uint128_t uint128_1;

// lhs type T as first arguemnt
// If the output is not a bool, casts to type T

// Bitwise Operators
template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
uint128_t operator&(const T & lhs, const uint128_t & rhs){
    return rhs & lhs;
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
T & operator&=(T & lhs, const uint128_t & rhs){
    return lhs = static_cast <T> (rhs & lhs);
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
uint128_t operator|(const T & lhs, const uint128_t & rhs){
    return rhs | lhs;
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
T & operator|=(T & lhs, const uint128_t & rhs){
    return lhs = static_cast <T> (rhs | lhs);
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
uint128_t operator^(const T & lhs, const uint128_t & rhs){
    return rhs ^ lhs;
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
T & operator^=(T & lhs, const uint128_t & rhs){
    return lhs = static_cast <T> (rhs ^ lhs);
}

// Bitshift operators
UINT128_T_EXTERN uint128_t operator<<(const bool     & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator<<(const uint8_t  & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator<<(const uint16_t & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator<<(const uint32_t & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator<<(const uint64_t & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator<<(const int8_t   & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator<<(const int16_t  & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator<<(const int32_t  & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator<<(const int64_t  & lhs, const uint128_t & rhs);

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
T & operator<<=(T & lhs, const uint128_t & rhs){
    return lhs = static_cast <T> (uint128_t(lhs) << rhs);
}

UINT128_T_EXTERN uint128_t operator>>(const bool     & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator>>(const uint8_t  & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator>>(const uint16_t & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator>>(const uint32_t & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator>>(const uint64_t & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator>>(const int8_t   & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator>>(const int16_t  & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator>>(const int32_t  & lhs, const uint128_t & rhs);
UINT128_T_EXTERN uint128_t operator>>(const int64_t  & lhs, const uint128_t & rhs);

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
T & operator>>=(T & lhs, const uint128_t & rhs){
    return lhs = static_cast <T> (uint128_t(lhs) >> rhs);
}

// Comparison Operators
template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
bool operator==(const T & lhs, const uint128_t & rhs){
    return rhs == lhs;
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
bool operator!=(const T & lhs, const uint128_t & rhs){
    return rhs != lhs;
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
bool operator>(const T & lhs, const uint128_t & rhs){
    return rhs < lhs;
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
bool operator<(const T & lhs, const uint128_t & rhs){
    return rhs > lhs;
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
bool operator>=(const T & lhs, const uint128_t & rhs){
    return rhs <= lhs;
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
bool operator<=(const T & lhs, const uint128_t & rhs){
    return rhs >= lhs;
}

// Arithmetic Operators
template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
uint128_t operator+(const T & lhs, const uint128_t & rhs){
    return rhs + lhs;
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
T & operator+=(T & lhs, const uint128_t & rhs){
    return lhs = static_cast <T> (rhs + lhs);
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
uint128_t operator-(const T & lhs, const uint128_t & rhs){
    return -(rhs - lhs);
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
T & operator-=(T & lhs, const uint128_t & rhs){
    return lhs = static_cast <T> (-(rhs - lhs));
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
uint128_t operator*(const T & lhs, const uint128_t & rhs){
    return rhs * lhs;
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
T & operator*=(T & lhs, const uint128_t & rhs){
    return lhs = static_cast <T> (rhs * lhs);
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
uint128_t operator/(const T & lhs, const uint128_t & rhs){
    return uint128_t(lhs) / rhs;
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
T & operator/=(T & lhs, const uint128_t & rhs){
    return lhs = static_cast <T> (uint128_t(lhs) / rhs);
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
uint128_t operator%(const T & lhs, const uint128_t & rhs){
    return uint128_t(lhs) % rhs;
}

template <typename T, typename = typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value, T>::type >
T & operator%=(T & lhs, const uint128_t & rhs){
    return lhs = static_cast <T> (uint128_t(lhs) % rhs);
}

// IO Operator
UINT128_T_EXTERN std::ostream & operator<<(std::ostream & stream, const uint128_t & rhs);
#endif
