#ifndef PLAYER_DEV_ZETMEDIA_ZETBUSMODULE_ZETLOGSERVERMDL_H
#define PLAYER_DEV_ZETMEDIA_ZETBUSMODULE_ZETLOGSERVERMDL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iZetModule.h"
#include "commonDefine.h"

class zetFeatureServerMdl: public iZetModule {
 public:
    zetFeatureServerMdl();
    void  init();
    bool  parseCmd(void* cmd);
    INT32 process(void* msg);
    INT32 processUpLayerCmd(const char* params, double seekTime);
    void  release();
    ~zetFeatureServerMdl();
 };
#endif

