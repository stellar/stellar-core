// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <vector>

namespace stellar
{

using std::vector;
using std::size_t;

template <typename T> class CircularBuffer
{
  public:
    explicit CircularBuffer(const size_t size)
        : buffer(size), start{0}, count{0}
    {
    }

    void
    push(T elem)
    {
        size_t size = buffer.size();
        size_t ix = (start + count) % size;
        if (count < size)
        {
            count += 1;
        }
        else
        {
            start = (start + 1) % size;
        }
        buffer[ix] = elem;
    }

    T&
    pop()
    {
        size_t size = buffer.size();
        size_t ix = start;
        start = (start + 1) % size;
        count -= 1;
        return buffer[ix];
    }

    bool
    empty() const
    {
        return 0 == count;
    }

  private:
    vector<T> buffer;
    size_t start;
    size_t count;
};
}
