#ifndef PLAYER_DEV_ZETMEDIA_IZETBUSFACTORY_H
#define PLAYER_DEV_ZETMEDIA_IZETBUSFACTORY_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iZetModule.h"
using namespace std;

class iZetModule;
class iZetBusFactory {
 public:
    virtual iZetModule* createZetModule(const char* moduleName) = 0;
    virtual ~iZetBusFactory() = default;
};

#endif

