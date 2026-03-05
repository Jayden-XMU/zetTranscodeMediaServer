#ifndef PLAYER_DEV_ZETMEDIA_ZETBUSMODULE_ZETTRANSCODEMDL_H
#define PLAYER_DEV_ZETMEDIA_ZETBUSMODULE_ZETTRANSCODEMDL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commonDefine.h"
#include "iZetModule.h"
#include "zetFFmpegProcess.h"

//struct _zetTranscodeArgs;
//struct _zetTranscodeParam;
struct _zetTransCodeInfo;

class zetTranscodeMdl: public iZetModule {
 public:
    zetTranscodeMdl();
    zetTranscodeMdl(const zetTranscodeMdl&) = delete;
    zetTranscodeMdl& operator=(const zetTranscodeMdl&) = delete;
    void  init();
    bool  parseCmd(void* cmd) ;
    INT32 process(void* msg);
#if FFMPEG_EXE_WITH_CMDLINE
    INT32 processWithCmdLine(void* msg);
#else
    INT32 processWithApi(void* msg);
    INT32 findBestStream(AVFormatContext* fmt_ctx, AVMediaType type, const std::string& type_name);
#endif

    INT32 processUpLayerCmd(const char* params, double seekTime);
    void  release();
    void  setCmdNum(INT32 &args);
    void  stopFFmpegProcess();
	
    ~zetTranscodeMdl();
 private: 
 	// struct _zetTranscodeArgs* transcodeArgs;
	// struct _zetTranscodeParam* transcodeParam;
    struct _zetTransCodeInfo* transcodeInfo; 
    INT32                     cmdNum;
    char*                     transCmdInfo;
#if FFMPEG_EXE_WITH_CMDLINE
    pid_t ffmpeg_pid;
#else 

#endif
 };
#endif

