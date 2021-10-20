// -*- C++ -*-
//===--------------------------- algorithm --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Subsequent modifications Copyright 2020 Stellar Development Foundation and
// contributors. Licensed under the Apache License, Version 2.0. See the COPYING
// file at the root of this distribution or at
// http://www.apache.org/licenses/LICENSE-2.0

#pragma once
#include <limits>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <type_traits>

#if defined(_MSC_VER)
#define _LIBCPP_COMPILER_MSVC
#endif

#if (defined(_M_AMD64) || defined(__x86_64__)) || \
    (defined(_M_ARM) || defined(__arm__))
#define _LIBCPP_HAS_BITSCAN64
#endif

#if defined(_LIBCPP_COMPILER_MSVC)
#include <intrin.h>
#define __CHAR_BIT__ 8
#endif

namespace stellar
{

using std::numeric_limits;
using std::true_type;
using std::false_type;
using std::conditional;
using std::iterator_traits;
using std::integral_constant;
using std::swap;

// This file contains extracts from libc++ <algoritm>, <random> and <bits> that
// provide randomness-related components of std:: (specifically
// uniform_int_distribution and shuffle) because these parts of the standard are
// underspecified and vary between stdlibs. We therefore commit specifically to
// some libc++ classes rather than using the stdlib-varying versions.

// __libcpp_clz

#ifndef _LIBCPP_COMPILER_MSVC

inline constexpr int
__libcpp_clz(unsigned __x) noexcept
{
    return __builtin_clz(__x);
}

inline constexpr int
__libcpp_clz(unsigned long __x) noexcept
{
    return __builtin_clzl(__x);
}

inline constexpr int
__libcpp_clz(unsigned long long __x) noexcept
{
    return __builtin_clzll(__x);
}

#else  // _LIBCPP_COMPILER_MSVC
// Precondition:  __x != 0
inline
int __libcpp_clz(unsigned __x) {
  static_assert(sizeof(unsigned) == sizeof(unsigned long), "");
  static_assert(sizeof(unsigned long) == 4, "");
  unsigned long __where;
  if (_BitScanReverse(&__where, __x))
    return static_cast<int>(31 - __where);
  return 32; // Undefined Behavior.
}

inline
int __libcpp_clz(unsigned long __x) {
    static_assert(sizeof(unsigned) == sizeof(unsigned long), "");
    return __libcpp_clz(static_cast<unsigned>(__x));
}

inline
int __libcpp_clz(unsigned long long __x) {
  unsigned long __where;
#if defined(_LIBCPP_HAS_BITSCAN64)
  if (_BitScanReverse64(&__where, __x))
    return static_cast<int>(63 - __where);
#else
  // Win32 doesn't have _BitScanReverse64 so emulate it with two 32 bit calls.
  if (_BitScanReverse(&__where, static_cast<unsigned long>(__x >> 32)))
    return static_cast<int>(63 - (__where + 32));
  if (_BitScanReverse(&__where, static_cast<unsigned long>(__x)))
    return static_cast<int>(63 - __where);
#endif
  return 64; // Undefined Behavior.
}

#endif


// __independent_bits_engine

template <unsigned long long _Xp, size_t _Rp>
struct __log2_imp
{
    static const size_t value = _Xp & ((unsigned long long)(1) << _Rp) ? _Rp
                                           : __log2_imp<_Xp, _Rp - 1>::value;
};

template <unsigned long long _Xp>
struct __log2_imp<_Xp, 0>
{
    static const size_t value = 0;
};

template <size_t _Rp>
struct __log2_imp<0, _Rp>
{
    static const size_t value = _Rp + 1;
};

template <class _UIntType, _UIntType _Xp>
struct __log2
{
    static const size_t value = __log2_imp<_Xp,
                                         sizeof(_UIntType) * __CHAR_BIT__ - 1>::value;
};

template<class _Engine, class _UIntType>
class __independent_bits_engine
{
public:
    // types
    typedef _UIntType result_type;

private:
    typedef typename _Engine::result_type _Engine_result_type;
    typedef typename conditional
        <
            sizeof(_Engine_result_type) <= sizeof(result_type),
                result_type,
                _Engine_result_type
        >::type _Working_result_type;

    _Engine& __e_;
    size_t __w_;
    size_t __w0_;
    size_t __n_;
    size_t __n0_;
    _Working_result_type __y0_;
    _Working_result_type __y1_;
    _Engine_result_type __mask0_;
    _Engine_result_type __mask1_;

    static constexpr const _Working_result_type _Rp = _Engine::max() - _Engine::min()
                                                      + _Working_result_type(1);
    static constexpr const size_t __m = __log2<_Working_result_type, _Rp>::value;
    static constexpr const size_t _WDt = numeric_limits<_Working_result_type>::digits;
    static constexpr const size_t _EDt = numeric_limits<_Engine_result_type>::digits;

public:
    // constructors and seeding functions
    __independent_bits_engine(_Engine& __e, size_t __w);

    // generating functions
    result_type operator()() {return __eval(integral_constant<bool, _Rp != 0>());}

private:
    result_type __eval(false_type);
    result_type __eval(true_type);
};

template<class _Engine, class _UIntType>
__independent_bits_engine<_Engine, _UIntType>
    ::__independent_bits_engine(_Engine& __e, size_t __w)
        : __e_(__e),
          __w_(__w)
{
    __n_ = __w_ / __m + (__w_ % __m != 0);
    __w0_ = __w_ / __n_;
    if (_Rp == 0)
        __y0_ = _Rp;
    else if (__w0_ < _WDt)
        __y0_ = (_Rp >> __w0_) << __w0_;
    else
        __y0_ = 0;
    if (_Rp - __y0_ > __y0_ / __n_)
    {
        ++__n_;
        __w0_ = __w_ / __n_;
        if (__w0_ < _WDt)
            __y0_ = (_Rp >> __w0_) << __w0_;
        else
            __y0_ = 0;
    }
    __n0_ = __n_ - __w_ % __n_;
    if (__w0_ < _WDt - 1)
        __y1_ = (_Rp >> (__w0_ + 1)) << (__w0_ + 1);
    else
        __y1_ = 0;
    __mask0_ = __w0_ > 0 ? _Engine_result_type(~0) >> (_EDt - __w0_) :
                          _Engine_result_type(0);
    __mask1_ = __w0_ < _EDt - 1 ?
                               _Engine_result_type(~0) >> (_EDt - (__w0_ + 1)) :
                               _Engine_result_type(~0);
}

template<class _Engine, class _UIntType>
inline
_UIntType
__independent_bits_engine<_Engine, _UIntType>::__eval(false_type)
{
    return static_cast<result_type>(__e_() & __mask0_);
}

template<class _Engine, class _UIntType>
_UIntType
__independent_bits_engine<_Engine, _UIntType>::__eval(true_type)
{
    const size_t _WRt = numeric_limits<result_type>::digits;
    result_type _Sp = 0;
    for (size_t __k = 0; __k < __n0_; ++__k)
    {
        _Engine_result_type __u;
        do
        {
            __u = __e_() - _Engine::min();
        } while (__u >= __y0_);
        if (__w0_ < _WRt)
            _Sp <<= __w0_;
        else
            _Sp = 0;
        _Sp += __u & __mask0_;
    }
    for (size_t __k = __n0_; __k < __n_; ++__k)
    {
        _Engine_result_type __u;
        do
        {
            __u = __e_() - _Engine::min();
        } while (__u >= __y1_);
        if (__w0_ < _WRt - 1)
            _Sp <<= __w0_ + 1;
        else
            _Sp = 0;
        _Sp += __u & __mask1_;
    }
    return _Sp;
}

// uniform_int_distribution

template<class _IntType = int>
class uniform_int_distribution
{
public:
    // types
    typedef _IntType result_type;

    class param_type
    {
        result_type __a_;
        result_type __b_;
    public:
        typedef uniform_int_distribution distribution_type;

        explicit param_type(result_type __a = 0,
                            result_type __b = numeric_limits<result_type>::max())
            : __a_(__a), __b_(__b) {}

        result_type a() const {return __a_;}
        result_type b() const {return __b_;}

        friend bool operator==(const param_type& __x, const param_type& __y)
            {return __x.__a_ == __y.__a_ && __x.__b_ == __y.__b_;}
        friend bool operator!=(const param_type& __x, const param_type& __y)
            {return !(__x == __y);}
    };

private:
    param_type __p_;

public:
    // constructors and reset functions
    explicit uniform_int_distribution(result_type __a = 0,
                                      result_type __b = numeric_limits<result_type>::max())
        : __p_(param_type(__a, __b)) {}
    explicit uniform_int_distribution(const param_type& __p) : __p_(__p) {}
    void reset() {}

    // generating functions
    template<class _URNG> result_type operator()(_URNG& __g)
        {return (*this)(__g, __p_);}
    template<class _URNG> result_type operator()(_URNG& __g, const param_type& __p);

    // property functions
    result_type a() const {return __p_.a();}
    result_type b() const {return __p_.b();}

    param_type param() const {return __p_;}
    void param(const param_type& __p) {__p_ = __p;}

    result_type min() const {return a();}
    result_type max() const {return b();}

    friend bool operator==(const uniform_int_distribution& __x,
                           const uniform_int_distribution& __y)
        {return __x.__p_ == __y.__p_;}
    friend bool operator!=(const uniform_int_distribution& __x,
                           const uniform_int_distribution& __y)
            {return !(__x == __y);}
};


template<class _IntType>
template<class _URNG>
typename uniform_int_distribution<_IntType>::result_type
uniform_int_distribution<_IntType>::operator()(_URNG& __g, const param_type& __p)
{
    typedef typename conditional<sizeof(result_type) <= sizeof(uint32_t),
                                            uint32_t, uint64_t>::type _UIntType;
    const _UIntType _Rp = _UIntType(__p.b()) - _UIntType(__p.a()) + _UIntType(1);
    if (_Rp == 1)
        return __p.a();
    const size_t _Dt = numeric_limits<_UIntType>::digits;
    typedef __independent_bits_engine<_URNG, _UIntType> _Eng;
    if (_Rp == 0)
        return static_cast<result_type>(_Eng(__g, _Dt)());
    size_t __w = _Dt - __libcpp_clz(_Rp) - 1;
    if ((_Rp & (numeric_limits<_UIntType>::max() >> (_Dt - __w))) != 0)
        ++__w;
    _Eng __e(__g, __w);
    _UIntType __u;
    do
    {
        __u = __e();
    } while (__u >= _Rp);
    return static_cast<result_type>(__u + __p.a());
}

template<class _RandomAccessIterator, class _UniformRandomNumberGenerator>
    void shuffle(_RandomAccessIterator __first, _RandomAccessIterator __last,
                 _UniformRandomNumberGenerator&& __g)
{
    typedef typename iterator_traits<_RandomAccessIterator>::difference_type difference_type;
    typedef uniform_int_distribution<ptrdiff_t> _Dp;
    typedef typename _Dp::param_type _Pp;
    difference_type __d = __last - __first;
    if (__d > 1)
    {
        _Dp __uid;
        for (--__last, (void) --__d; __first < __last; ++__first, (void) --__d)
        {
            difference_type __i = __uid(__g, _Pp(0, __d));
            if (__i != difference_type(0))
                swap(*__first, *(__first + __i));
        }
    }
}
}
