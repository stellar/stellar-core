#include <iostream>
#include <cstring>

int main() {
    char buffer[10];
    std::strcpy(buffer, "This string is way too long for the buffer!");
    std::cout << "Buffer content: " << buffer << std::endl;
    return 0;
}
