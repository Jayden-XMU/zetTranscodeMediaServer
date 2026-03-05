#ifndef PLAYER_DEV_ZETMEDIA_ZETBUSFACTORY_H
#define PLAYER_DEV_ZETMEDIA_ZETBUSFACTORY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iZetBusFactory.h"

typedef enum _zetMdlName {
    ZETCTRLMODULE,
    ZETHLSSERVERMODULE,
    ZETTRANSCODEMODULE,
    ZETFEATURESERVERMODULE
} zetMdlName;

class iZetModule;
class zetBusFactory : public iZetBusFactory {
public:
    zetBusFactory() = default;
    iZetModule* createZetModule(const char* moduleName) override;
   ~zetBusFactory() = default;
};

#endif

