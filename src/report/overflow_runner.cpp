#include "overflow.h"
#include <iostream>

int
main()
{
    std::cout << "Starte Overflow-Test..." << std::endl;
    triggerOverflow();
    std::cout << "Test abgeschlossen." << std::endl;
    return 0;
}