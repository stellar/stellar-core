#include <cstdint>
#include <cstddef>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char buffer[10];
    if (size < 50) {
        std::strcpy(buffer, reinterpret_cast<const char*>(data)); // intentional overflow
    }
    return 0;
}
