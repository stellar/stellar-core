#pragma once
namespace stellar
{

struct NonCopyable
{
    NonCopyable() = default;
    NonCopyable(NonCopyable const&) = delete;
    NonCopyable& operator=(NonCopyable const&) = delete;
};

struct NonMovable
{
    NonMovable() = default;
    NonMovable(NonMovable&&) = delete;
    NonMovable& operator=(NonMovable&&) = delete;
};

struct NonMovableOrCopyable : private NonCopyable, NonMovable
{
    NonMovableOrCopyable() = default;
};
}
