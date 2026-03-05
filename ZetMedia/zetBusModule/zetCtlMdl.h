#ifndef PLAYER_DEV_ZETMEDIA_ZETBUSMODULE_ZETCTLMDL_H
#define PLAYER_DEV_ZETMEDIA_ZETBUSMODULE_ZETCTLMDL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commonDefine.h"
#include "iZetModule.h"
#include "zetBusFactory.h"

typedef enum _commandType {
    ZET_CONTROL,
    ZET_TRANSCODE,
    ZET_HLS_GENERATE,
    ZET_LOG_SERVER,
    ZET_UNINITIALED,
} commandType;

class zetHlsServerMdl;
class zetFeatureServerMdl;
class zetTranscodeMdl;
class iZetModule;

struct _zetCmdMsg;
class zetCtlMdl : public iZetModule {
  public: 
     zetCtlMdl();
     void        init();
     bool        registerCtlObj(iZetModule*mod, zetMdlName name);
     iZetModule* getCtlObj(zetMdlName name);
     INT32       preParseCmd(int argc, char* argv[]);
     bool        parseCmd(void*cmd);
     INT32       process(void* msg);
     INT32       processUpLayerCmd(const char* params, double seekTime);
     void        release();
     ~zetCtlMdl();
  private:
    zetHlsServerMdl*        hlsServerMdl;
    zetFeatureServerMdl*    featureServerMdl;
    zetTranscodeMdl*        transcodeMdl;
    commandType             cmdType;
    struct _zetCmdMsg *     cmdMsg;
    char*                   cmdInfo;
    INT32                   cmdNum;
 };
 
#endif

