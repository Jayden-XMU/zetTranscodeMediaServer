#ifndef PLAYER_DEV_ZETMEDIA_ZETBUSMODULE_IZETMODULE_H
#define PLAYER_DEV_ZETMEDIA_ZETBUSMODULE_IZETMODULE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commonDefine.h"

class iZetModule {
 public: 
    virtual void  init() = 0;
    virtual bool  parseCmd(void* cmd) = 0;
    virtual INT32 process(void* msg) = 0;
    virtual INT32 processUpLayerCmd(const char* params, double seekTime) = 0; 
    virtual void  release() = 0;
};

#endif

