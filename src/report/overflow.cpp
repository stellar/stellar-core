#include "overflow.h"
#include <cstring>

void triggerOverflow() {
    char buffer[8];
    std::strcpy(buffer, "This is too long for buffer"); // absichtlicher Overflow
}

