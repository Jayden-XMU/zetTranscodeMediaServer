#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zetCtlMdl.h"
#include "zetHlsServerMdl.h"
#include "zetFeatureServerMdl.h"
#include "zetTranscodeMdl.h"

using namespace std;

typedef struct _zetCmdMsg {
    char            file_input[MAX_FILE_NAME_LENGTH];
    char            file_output[MAX_FILE_NAME_LENGTH];
    char            audCodingType[MAX_TYPE_NAME_LENGTH];
    char            vidCodingType[MAX_TYPE_NAME_LENGTH];
    char            fileType[MAX_TYPE_NAME_LENGTH];
    float           framerate;
    UINT32          width;
    UINT32          height;
    UINT32          bitrate;
    UINT32          sampleRate;
    UINT32          channels;
    bool            need_input;
    bool            need_output;
    commandType     cmdType;
} zetCmdMsg;

zetCtlMdl::zetCtlMdl() {
    this->init();
}

void zetCtlMdl::init() {
    cmdNum           = 0;
    hlsServerMdl     = NULL;
    featureServerMdl = NULL;
    transcodeMdl     = NULL;
    cmdType          = ZET_UNINITIALED;
    cmdMsg           = (zetCmdMsg*)calloc(1, sizeof(zetCmdMsg));
    if (!cmdMsg) {
        LOG(LOG_ERROR, "Memory allocation failed!!!");
        return;
    }
    LOG(LOG_DEBUG, "called...");
}

INT32 zetCtlMdl::preParseCmd(int argc, char* argv[]) {
    const char *opt;
    const char *next;
    INT32 optindex      = 1;
    INT32 handleoptions = 1;
    INT32 err           = ZET_NOK;
    cmdNum              = argc;
    if ((argc < 2) || (cmdMsg == NULL)) {
        LOG(LOG_ERROR, "input parameters is not right, input num: %d msg: %p please check!!!", argc, cmdMsg);
        optionsExplain();
        return err;
    }

    // start to parse options
    while (optindex < argc) {
        opt  = (const char*)argv[optindex++];
        next = (const char*)argv[optindex];

        if (handleoptions && opt[0] == '-' && opt[1] != '\0') {
            if (opt[1] == '-') {
                if (opt[2] != '\0') {
                    opt++;
                } else {
                    handleoptions = 0;
                    continue;
                }
            }
            opt++;
       	}
        const char targetchar[] = "hls";
        if (next && strstr(next, targetchar)) {
            LOG(LOG_DEBUG, "hls server command found, argv: %s!!!", next);
            cmdType = ZET_HLS_GENERATE;
            goto PARSE_OPINIONS_OUT;
        }
    }
    cmdType = ZET_TRANSCODE;
PARSE_OPINIONS_OUT:
    if (this->parseCmd(argv)) {
        err = ZET_OK;
    }
    return err;
}

bool zetCtlMdl::parseCmd(void*cmd) {
    ZETCHECK_PTR_IS_NULL(cmd);
    bool ret = ZET_FALSE;
    switch (cmdType) {
        case ZET_TRANSCODE:
            ZETCHECK_PTR_IS_NULL(transcodeMdl);
            transcodeMdl->setCmdNum(cmdNum);
            ret = transcodeMdl->parseCmd(cmd);
            break;
        case ZET_HLS_GENERATE:
            ZETCHECK_PTR_IS_NULL(hlsServerMdl);
            hlsServerMdl->setCmdNum(cmdNum);
            ret = hlsServerMdl->parseCmd(cmd);
            break;
        case ZET_LOG_SERVER:
            ZETCHECK_PTR_IS_NULL(featureServerMdl);
            ret = featureServerMdl->parseCmd(cmd);
            break;
        case ZET_CONTROL:
            ret = this->process(cmd);
            break;
        default:
            break;
	}
    return ZET_TRUE;
}

INT32 zetCtlMdl::process(void* msg) {
    INT32 ret = ZET_NOK;
    switch (cmdType) {
        case ZET_TRANSCODE:
            ZETCHECK_PTR_IS_NULL(transcodeMdl);
            ret = transcodeMdl->process(msg);
            break;
        case ZET_HLS_GENERATE:
            ZETCHECK_PTR_IS_NULL(hlsServerMdl);
            ret = hlsServerMdl->process(msg);
            break;
        case ZET_LOG_SERVER:
            ZETCHECK_PTR_IS_NULL(featureServerMdl);
            ret = featureServerMdl->process(msg);
            break;
        case ZET_CONTROL:
            // doing process in this module here...
            break;
        default:
            break;
	}
    return ret;
}

INT32 zetCtlMdl::processUpLayerCmd(const char* params, double seekTime) {
    INT32 ret = ZET_NOK;
    switch (cmdType) {
        case ZET_TRANSCODE:
            ZETCHECK_PTR_IS_NULL(transcodeMdl);
            ret = transcodeMdl->processUpLayerCmd(params, seekTime);
            break;
        case ZET_HLS_GENERATE:
            ZETCHECK_PTR_IS_NULL(hlsServerMdl);
            ret = hlsServerMdl->processUpLayerCmd(params, seekTime);
            break;
        case ZET_LOG_SERVER:
            ZETCHECK_PTR_IS_NULL(featureServerMdl);
            ret = featureServerMdl->processUpLayerCmd(params, seekTime);
            break;
        case ZET_CONTROL:
            // doing process in this module here...
            break;
        default:
            break;
    }

    if (!strcasecmp(params, "stop")) {
        this->release();
        ret = ZET_OK;
    }

    LOG(LOG_DEBUG, "called, and exit...");
    return ret;
}


void zetCtlMdl::release() {
    if (hlsServerMdl) {
        hlsServerMdl->stopFFmpegProcess();
    }

    if (transcodeMdl) {
        transcodeMdl->stopFFmpegProcess();
    }

    SAFE_FREE(cmdMsg);
    SAFE_DELETE(hlsServerMdl);
    SAFE_DELETE(featureServerMdl);
    SAFE_DELETE(transcodeMdl);
    LOG(LOG_DEBUG, "called, and exit...");
}

bool zetCtlMdl::registerCtlObj(iZetModule* mod, zetMdlName name) {
    if (mod == NULL) {
        LOG(LOG_ERROR, "target module is NULL, quit!!!");
        return ZET_FALSE;
    }

    switch (name) {
        case ZETHLSSERVERMODULE:
            hlsServerMdl = dynamic_cast<zetHlsServerMdl*>(mod);
            ZETCHECK_PTR_IS_NULL(hlsServerMdl);
            break;
        case ZETFEATURESERVERMODULE:
            featureServerMdl = dynamic_cast<zetFeatureServerMdl*>(mod);
            ZETCHECK_PTR_IS_NULL(featureServerMdl);
            break;
        case ZETTRANSCODEMODULE:
            transcodeMdl = dynamic_cast<zetTranscodeMdl*>(mod);
            ZETCHECK_PTR_IS_NULL(transcodeMdl);
            break;
        default:
            LOG(LOG_ERROR, "unknown module found, please check!!!");
            break;
	}
    return ZET_TRUE;
}

iZetModule* zetCtlMdl::getCtlObj(zetMdlName name) {
    switch (name) {
        case ZETHLSSERVERMODULE:
            return (!hlsServerMdl) ? NULL: hlsServerMdl;
        case ZETFEATURESERVERMODULE:
            return (!featureServerMdl) ? NULL: featureServerMdl;
        case ZETTRANSCODEMODULE:
            return (!transcodeMdl) ? NULL: transcodeMdl;
        default:
            LOG(LOG_ERROR, "unknown module found, please check!!!");
            return NULL;
	}
}

zetCtlMdl::~zetCtlMdl() {
    this->release();
    LOG(LOG_DEBUG, "called, and exit...");
}

