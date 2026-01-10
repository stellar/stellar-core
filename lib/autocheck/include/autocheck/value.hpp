#ifndef AUTOCHECK_VALUE_HPP
#define AUTOCHECK_VALUE_HPP

#include <autocheck/ostream.hpp>

#include <cassert>

template <typename T>
void unused(T& x) {}

namespace autocheck {

  template <typename T>
  class value {
    private:  
      // Visual Studio before 2015 doesn't support unrestricted unions.
      // However, if we declare `T object` as a member,
      // then we must declare the allocation as `Static`,
      // or else `object` will leak when we start to generate test values.
      enum {
        None,
        Static,
        Heap
      }    allocation =
#if !defined(_MSC_VER) || _MSC_VER >= 1900
                        None;
#else
                        Static;
#endif

#if !defined(_MSC_VER) || _MSC_VER >= 1900
      union {
#endif
        T* pointer = nullptr;
        T  object;
#if !defined(_MSC_VER) || _MSC_VER >= 1900
      };
#endif

    public:
      value() {}

      value(const value& copy) { *this = copy; }

      value& operator= (const value& rhs) {
        if (this == &rhs) return *this;

        if (rhs.allocation == Static) {
          construct(rhs.cref());
        } else if (rhs.allocation == Heap) {
          ptr(new T(rhs.cref()));
        }

        return *this;
      }

      value& operator= (const T& rhs) {
        construct(rhs);
        return *this;
      }

      value& operator= (T* rhs) {
        ptr(rhs);
        return *this;
      }

      bool empty() const { return allocation == None; }

      template <typename... Args>
      void construct(const Args&... args) {
        clear();
        T* p = new (&object) T(args...);
        assert(p == &object);
        unused(p);
        allocation = Static;
      }

      const T* ptr() const {
        return (allocation == Heap) ? pointer : &object;
      }

      T* ptr() {
        return (allocation == Heap) ? pointer : &object;
      }

      void ptr(T* p) {
        clear();
        pointer    = p;
        allocation = p ? Heap : None;
      }

      T* operator-> ()             { return ptr(); }
      const T* operator-> () const { return ptr(); }

      T& ref()              { return *ptr(); }
      const T& ref()  const { return *ptr(); }
      const T& cref() const { return *ptr(); }

      operator T& ()             { return ref(); }
      operator const T& () const { return cref(); }

      void clear() {
        if (allocation == Heap) {
          delete ptr();
        } else if (allocation == Static) {
          ptr()->~T();
        }
        allocation = None;
      }

      ~value() { clear(); }

  };

  template <typename T>
  std::ostream& operator<< (std::ostream& out, const value<T>& v) {
    return out << v.cref();
  }

}

#endif

