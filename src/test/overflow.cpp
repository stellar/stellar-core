#include <iostream>
#include <cstring>

void runOverflowTest() {
    char buffer[10];
    std::strcpy(buffer, "This string is way too long for the buffer!");
    std::cout << "Buffer content: " << buffer << std::endl;
}
