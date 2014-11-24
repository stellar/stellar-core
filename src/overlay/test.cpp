
#include <iostream>

struct foo {
    static constexpr int yes() { return 1; }
};

int
main()
{
    std::cout << "hello world\n";
}