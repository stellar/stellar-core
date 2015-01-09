/*
uint128_t.h
An unsigned 128 bit integer type for C++
Copyright (c) 2014 Jason Lee @ calccrypto at gmail.com

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

Thanks to Stefan Deigm�ller for finding
a bug in operator*.

Thanks to Fran�ois Dessenne for convincing me
to do a general rewrite of this class.
*/

#ifndef __UINT128_T__
#define __UINT128_T__

#include <iostream>
#include <stdexcept>
#include <stdint.h>
#include <utility>
#include <string>

class uint128_t{
    private:
        uint64_t UPPER, LOWER;

    public:
        // Constructors
        uint128_t();
        uint128_t(const uint128_t & rhs);

        template <typename T> explicit uint128_t(const T & rhs){
            UPPER = 0;
            LOWER = rhs;
        }

        template <typename S, typename T> uint128_t(const S & upper_rhs, const T & lower_rhs){
            UPPER = upper_rhs;
            LOWER = lower_rhs;
        }

        //  RHS input args only

        // Assignment Operator
        uint128_t operator=(const uint128_t & rhs);

        template <typename T> uint128_t operator=(const T & rhs){
            UPPER = 0;
            LOWER = rhs;
            return *this;
        }

        // Typecast Operators
        operator bool() const;
        operator char() const;
        operator int() const;
        operator uint8_t() const;
        operator uint16_t() const;
        operator uint32_t() const;
        operator uint64_t() const;

        // Bitwise Operators
        uint128_t operator&(const uint128_t & rhs) const;
        uint128_t operator|(const uint128_t & rhs) const;
        uint128_t operator^(const uint128_t & rhs) const;
        uint128_t operator&=(const uint128_t & rhs);
        uint128_t operator|=(const uint128_t & rhs);
        uint128_t operator^=(const uint128_t & rhs);
        uint128_t operator~() const;

        template <typename T> uint128_t operator&(const T & rhs) const{
            return uint128_t(0, LOWER & (uint64_t) rhs);
        }

        template <typename T> uint128_t operator|(const T & rhs) const{
            return uint128_t(UPPER, LOWER | (uint64_t) rhs);
        }

        template <typename T> uint128_t operator^(const T & rhs) const{
            return uint128_t(UPPER, LOWER ^ (uint64_t) rhs);
        }

        template <typename T> uint128_t operator&=(const T & rhs){
            UPPER = 0;
            LOWER &= rhs;
            return *this;
        }

        template <typename T> uint128_t operator|=(const T & rhs){
            LOWER |= (uint64_t) rhs;
            return *this;
        }

        template <typename T> uint128_t operator^=(const T & rhs){
            LOWER ^= (uint64_t) rhs;
            return *this;
        }

        // Bit Shift Operators
        uint128_t operator<<(const uint128_t & rhs) const;
        uint128_t operator>>(const uint128_t & rhs) const;
        uint128_t operator<<=(const uint128_t & rhs);
        uint128_t operator>>=(const uint128_t & rhs);

        template <typename T>uint128_t operator<<(const T & rhs) const{
            return *this << uint128_t(rhs);
        }

        template <typename T>uint128_t operator>>(const T & rhs) const{
            return *this >> uint128_t(rhs);
        }

        template <typename T>uint128_t operator<<=(const T & rhs){
            *this = *this << uint128_t(rhs);
            return *this;
        }

        template <typename T>uint128_t operator>>=(const T & rhs){
            *this = *this >> uint128_t(rhs);
            return *this;
        }

        // Logical Operators
        bool operator!() const;
        bool operator&&(const uint128_t & rhs) const;
        bool operator||(const uint128_t & rhs) const;

        template <typename T> bool operator&&(const T & rhs){
            return ((bool) *this && rhs);
        }

        template <typename T> bool operator||(const T & rhs){
            return ((bool) *this || rhs);
        }

        // Comparison Operators
        bool operator==(const uint128_t & rhs) const;
        bool operator!=(const uint128_t & rhs) const;
        bool operator>(const uint128_t & rhs) const;
        bool operator<(const uint128_t & rhs) const;
        bool operator>=(const uint128_t & rhs) const;
        bool operator<=(const uint128_t & rhs) const;

        template <typename T> bool operator==(const T & rhs) const{
            return (!UPPER && (LOWER == (uint64_t) rhs));
        }

        template <typename T> bool operator!=(const T & rhs) const{
            return (UPPER | (LOWER != (uint64_t) rhs));
        }

        template <typename T> bool operator>(const T & rhs) const{
            return (UPPER || (LOWER > (uint64_t) rhs));
        }

        template <typename T> bool operator<(const T & rhs) const{
            return (!UPPER)?(LOWER < (uint64_t) rhs):false;
        }

        template <typename T> bool operator>=(const T & rhs) const{
            return ((*this > rhs) | (*this == rhs));
        }

        template <typename T> bool operator<=(const T & rhs) const{
            return ((*this < rhs) | (*this == rhs));
        }

        // Arithmetic Operators
        uint128_t operator+(const uint128_t & rhs) const;
        uint128_t operator+=(const uint128_t & rhs);
        uint128_t operator-(const uint128_t & rhs) const;
        uint128_t operator-=(const uint128_t & rhs);
        uint128_t operator*(const uint128_t & rhs) const;
        uint128_t operator*=(const uint128_t & rhs);

    private:
        std::pair <uint128_t, uint128_t> divmod(const uint128_t & lhs, const uint128_t & rhs) const;

    public:
		uint128_t operator/(const uint128_t & rhs) const;
        uint128_t operator/=(const uint128_t & rhs);
        uint128_t operator%(const uint128_t & rhs) const;
        uint128_t operator%=(const uint128_t & rhs);

        template <typename T> uint128_t operator+(const T & rhs) const{
            return uint128_t(UPPER + ((LOWER + (uint64_t) rhs) < LOWER), LOWER + (uint64_t) rhs);
        }

        template <typename T> uint128_t operator+=(const T & rhs){
            UPPER = UPPER + ((LOWER + rhs) < LOWER);
            LOWER = LOWER + rhs;
            return *this;
        }

        template <typename T> uint128_t operator-(const T & rhs) const{
            return uint128_t((uint64_t) (UPPER - ((LOWER - rhs) > LOWER)), (uint64_t) (LOWER - rhs));
        }

        template <typename T> uint128_t operator-=(const T & rhs){
            *this = *this - rhs;
            return *this;
        }

        template <typename T> uint128_t operator*(const T & rhs) const{
            return (*this) * (uint128_t(rhs));
        }

        template <typename T> uint128_t operator*=(const T & rhs){
            *this = *this * uint128_t(rhs);
            return *this;
        }

        template <typename T> uint128_t operator/(const T & rhs) const{
            return *this / uint128_t(rhs);
        }

        template <typename T> uint128_t operator/=(const T & rhs){
            *this = *this / uint128_t(rhs);
            return *this;
        }

        template <typename T> uint128_t operator%(const T & rhs) const{
            return *this - (rhs * (*this / rhs));
        }

        template <typename T> uint128_t operator%=(const T & rhs){
            *this = *this % uint128_t(rhs);
            return *this;
        }

        // Increment Operator
        uint128_t operator++();
        uint128_t operator++(int);

        // Decrement Operator
        uint128_t operator--();
        uint128_t operator--(int);

        // Get private values
        uint64_t upper() const;
        uint64_t lower() const;

        // Get bitsize of value
        uint8_t bits() const;

        // Get string representation of value
        std::string str(uint8_t base = 10, const unsigned int & len = 0) const;
};

// Useful values
extern const uint128_t uint128_0;
extern const uint128_t uint128_1;
extern const uint128_t uint128_64;
extern const uint128_t uint128_128;

// lhs type T as first arguemnt
// If the output is not a bool, casts to type T

// Bitwise Operators
template <typename T> T operator&(const T & lhs, const uint128_t & rhs){
    return (T) (lhs & (T) rhs.lower());
}

template <typename T> T operator|(const T & lhs, const uint128_t & rhs){
    return (T) (lhs | (T) rhs.lower());
}

template <typename T> T operator^(const T & lhs, const uint128_t & rhs){
    return (T) (lhs ^ (T) rhs.lower());
}

template <typename T> T operator&=(T & lhs, const uint128_t & rhs){
    lhs &= (T) rhs.lower(); return lhs;
}

template <typename T> T operator|=(T & lhs, const uint128_t & rhs){
    lhs |= (T) rhs.lower(); return lhs;
}

template <typename T> T operator^=(T & lhs, const uint128_t & rhs){
    lhs ^= (T) rhs.lower(); return lhs;
}

// Comparison Operators
template <typename T> bool operator==(const T & lhs, const uint128_t & rhs){
    return (!rhs.upper() && ((uint64_t) lhs == rhs.lower()));
}

template <typename T> bool operator!=(const T & lhs, const uint128_t & rhs){
    return (rhs.upper() | ((uint64_t) lhs != rhs.lower()));
}

template <typename T> bool operator>(const T & lhs, const uint128_t & rhs){
    return (!rhs.upper()) && ((uint64_t) lhs > rhs.lower());
}

template <typename T> bool operator<(const T & lhs, const uint128_t & rhs){
    if (rhs.upper()){
        return true;
    }
    return ((uint64_t) lhs < rhs.lower());
}

template <typename T> bool operator>=(const T & lhs, const uint128_t & rhs){
    if (rhs.upper()){
            return false;
    }
    return ((uint64_t) lhs >= rhs.lower());
}

template <typename T> bool operator<=(const T & lhs, const uint128_t & rhs){
    if (rhs.upper()){
            return true;
    }
    return ((uint64_t) lhs <= rhs.lower());
}

// Arithmetic Operators
template <typename T> T operator+(const T & lhs, const uint128_t & rhs){
    return (T) (rhs + lhs);
}

template <typename T> T & operator+=(T & lhs, const uint128_t & rhs){
    lhs = (T) (rhs + lhs);
    return lhs;
}

template <typename T> T operator-(const T & lhs, const uint128_t & rhs){
    return (T) (uint128_t(lhs) - rhs);
}

template <typename T> T & operator-=(T & lhs, const uint128_t & rhs){
    lhs = (T) (uint128_t(lhs) - rhs);
    return lhs;
}

template <typename T> T operator*(const T & lhs, const uint128_t & rhs){
    return lhs * (T) rhs.lower();
}

template <typename T> T & operator*=(T & lhs, const uint128_t & rhs){
    lhs *= (T) rhs.lower();
    return lhs;
}

template <typename T> T operator/(const T & lhs, const uint128_t & rhs){
    return (T) (uint128_t(lhs) / rhs);
}

template <typename T> T & operator/=(T & lhs, const uint128_t & rhs){
    lhs = (T) (uint128_t(lhs) / rhs);
    return lhs;
}

template <typename T> T operator%(const T & lhs, const uint128_t & rhs){
    return (T) (uint128_t(lhs) % rhs);
}

template <typename T> T & operator%=(T & lhs, const uint128_t & rhs){
    lhs = (T) (uint128_t(lhs) % rhs);
    return lhs;
}

// IO Operator
std::ostream & operator<<(std::ostream & stream, const uint128_t & rhs);
#endif
