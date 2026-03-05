#include <iostream>
#include <cstring>

#include "zetBusFactory.h"
#include "zetCtlMdl.h"
#include "zetHlsServerMdl.h"
#include "zetFeatureServerMdl.h"
#include "zetTranscodeMdl.h"
#include "iZetModule.h"

using namespace std;

iZetModule* zetBusFactory::createZetModule(const char* moduleName) {
    if (moduleName == NULL) {
        LOG(LOG_ERROR, "no specified moduleName created, return...");
        return NULL;
    }
    if(!strcmp(moduleName, "zetControlModule")) {
        iZetModule* ctlMdl = new zetCtlMdl(); 
        return ctlMdl;
    } else if (!strcmp(moduleName, "zetTranscodeMdl")) {
        iZetModule* transcodeMdl = new zetTranscodeMdl(); 
        return transcodeMdl;
    } else if (!strcmp(moduleName, "zetHlsServerMdl")) {
        iZetModule* hlsServerMdl = new zetHlsServerMdl(); 
        return hlsServerMdl;
    } else if (!strcmp(moduleName, "zetFeatureServerMdl")) {
        iZetModule* featureServerMdl = new zetFeatureServerMdl();
        return featureServerMdl;
    } else {
        LOG(LOG_ERROR, "other specified moduleName found, please check!!!");
        return NULL;
    }
}


