// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/Catch2.h"
#include "util/StackThread.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

using stellar::StackThread;

namespace
{

static void
freeFn(std::atomic<int>* counter, int a, int b)
{
    *counter += a + b;
}

struct Functor
{
    std::atomic<int>* mCounter;

    void
    operator()(std::string s) const
    {
        *mCounter += static_cast<int>(s.size());
    }
};

struct MoveOnly
{
    std::unique_ptr<int> p;
};

bool
waitUntil(std::function<bool()> const& pred)
{
    auto const deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pred())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

}

TEST_CASE("StackThread invokes free function with arguments", "[stackthread]")
{
    std::atomic<int> counter{0};
    StackThread t(1 << 20, freeFn, &counter, 3, 4);
    REQUIRE(t.joinable());
    t.join();
    REQUIRE_FALSE(t.joinable());
    REQUIRE(counter == 7);
}

#if defined(__linux__)
TEST_CASE("StackThread applies requested stack size and name", "[stackthread]")
{
    std::size_t observed = 0;
    std::string name;
    StackThread t(4 * 1024 * 1024, "deep-worker", [&observed, &name] {
        pthread_attr_t a;
        if (pthread_getattr_np(pthread_self(), &a) == 0)
        {
            void* base = nullptr;
            pthread_attr_getstack(&a, &base, &observed);
            pthread_attr_destroy(&a);
        }
        char nameBuf[32] = {0};
        pthread_getname_np(pthread_self(), nameBuf, sizeof(nameBuf));
        name = nameBuf;
    });
    t.join();
    REQUIRE(observed == 4u * 1024 * 1024);
    REQUIRE(name == "deep-worker");
}
#endif

TEST_CASE("StackThread invokes functor with by-value string", "[stackthread]")
{
    std::atomic<int> counter{0};
    StackThread t(0, Functor{&counter}, std::string("hello"));
    t.join();
    REQUIRE(counter == 5);
}

TEST_CASE("StackThread invokes callable with move-only argument",
          "[stackthread]")
{
    std::atomic<int> counter{0};
    MoveOnly m{std::make_unique<int>(42)};
    StackThread t(
        1 << 16, [&counter](MoveOnly mo) { counter += *mo.p; }, std::move(m));
    t.join();
    REQUIRE(counter == 42);
}

TEST_CASE("StackThread invokes member function pointer", "[stackthread]")
{
    std::atomic<int> counter{0};
    struct S
    {
        std::atomic<int>* mCounter;
        int v = 5;
        void
        bump(int n)
        {
            *mCounter += v * n;
        }
    } s{&counter};
    StackThread t(1 << 16, &S::bump, &s, 2);
    t.join();
    REQUIRE(counter == 10);
}

TEST_CASE("StackThread supports move construction assignment and swap",
          "[stackthread]")
{
    StackThread a(1 << 16, [] {});
    auto aid = a.get_id();
    StackThread b(std::move(a));
    REQUIRE_FALSE(a.joinable());
    REQUIRE(b.joinable());
    REQUIRE(b.get_id() == aid);
    StackThread c;
    c = std::move(b);
    REQUIRE(c.joinable());
    REQUIRE_FALSE(b.joinable());
    swap(c, b);
    REQUIRE(b.joinable());
    REQUIRE_FALSE(c.joinable());
    b.join();
}

TEST_CASE("StackThread id supports default construction lookup and streaming",
          "[stackthread]")
{
    StackThread::id d1, d2;
    REQUIRE(d1 == d2);
    StackThread t(1 << 16, [] {});
    REQUIRE(t.get_id() != d1);
    std::map<StackThread::id, int> m;
    std::unordered_map<StackThread::id, int> um;
    m[t.get_id()] = 1;
    um[t.get_id()] = 1;
    REQUIRE(m.count(t.get_id()) == 1);
    REQUIRE(um.count(t.get_id()) == 1);
    std::ostringstream os;
    os << t.get_id();
    REQUIRE_FALSE(os.str().empty());
    t.join();
    REQUIRE(t.get_id() == d1);
}

#if !defined(_WIN32)
TEST_CASE("StackThread native handle is usable while thread is running",
          "[stackthread]")
{
    std::atomic<bool> go{false}, up{false};
    StackThread t(1 << 16, [&] {
        up = true;
        while (!go)
        {
            std::this_thread::yield();
        }
    });
    REQUIRE(waitUntil([&] { return up.load(); }));

    int policy = 0;
    sched_param sp{};
    int rc = pthread_getschedparam(t.native_handle(), &policy, &sp);
    REQUIRE(rc == 0);

#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    rc = pthread_setaffinity_np(t.native_handle(), sizeof(set), &set);
    CHECK(rc != ESRCH);
#endif

    go = true;
    t.join();
}
#endif

TEST_CASE("StackThread supports detach", "[stackthread]")
{
    std::atomic<bool> done{false};
    StackThread t(1 << 16, [&done] { done = true; });
    t.detach();
    REQUIRE_FALSE(t.joinable());
    REQUIRE(waitUntil([&] { return done.load(); }));
}

TEST_CASE("StackThread join on non-joinable throws", "[stackthread]")
{
    StackThread t;
    REQUIRE_THROWS_AS(t.join(), std::system_error);
}

TEST_CASE("StackThread clamps tiny stack requests to platform minimum",
          "[stackthread]")
{
    StackThread t(1, [] {});
    t.join();
}