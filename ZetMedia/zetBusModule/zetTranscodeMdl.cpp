#include <iostream>
#include <cstdlib>

#include "zetTranscodeMdl.h"
using namespace std;

typedef struct _zetTranscodeArgs{    
    const char *input_filename;
    const char *output_filename;
    const char *format_name;
    int         progress;
    int         completed;  
    char        error_msg[256];
} zetTranscodeArgs;

typedef struct _zetTranscodeParam {
    INT32  videoCodeID;  
    INT32  videoWidth;
    INT32  videoHeight;     
    float  videoFrameRate;
    INT32  audioCodecID;
    INT32  audioChannels;
    INT32  audioSampleRate;
    char*  fileType;
    INT64  videoBitrate;
} zetTranscodeParam; 

typedef struct _zetTransCodeInfo {
    char            file_input[MAX_FILE_NAME_LENGTH];
    char            file_output[MAX_FILE_NAME_LENGTH];
    char            audCodingType[MAX_TYPE_NAME_LENGTH];
    char            vidCodingType[MAX_TYPE_NAME_LENGTH];
    char            fileType[MAX_TYPE_NAME_LENGTH];
    float           framerate;
    UINT32          width;
    UINT32          height;
    UINT32          bitrate;
    UINT32          audBitrate;
    UINT32          threads;
    UINT32          sampleRate;
    UINT32          channels;
    bool            need_input;
    bool            need_output;
    bool            lowLatency;
} zetTransCodeInfo;

zetTranscodeMdl::zetTranscodeMdl(): transCmdInfo(NULL), transcodeInfo(NULL) {
    this->init();
}

void zetTranscodeMdl::init() {
#if FFMPEG_EXE_WITH_CMDLINE
    transCmdInfo  = NULL;
    transcodeInfo = (zetTransCodeInfo*)calloc(1, sizeof(zetTransCodeInfo));
    if (!transcodeInfo) {
        LOG(LOG_ERROR, "Memory allocation failed!!!");
        return;
    }
    ffmpeg_pid = -1;
    LOG(LOG_DEBUG, "called...");
#else 
    
#endif
}

void zetTranscodeMdl::setCmdNum(INT32& args) {
    cmdNum = args;
}

static void showTransCodeCmdInfo(int argc, char*argv[]) {
    LOG(LOG_DEBUG, "the full input command info is: ");
    for (int i = 0; i< argc; i++) {
        printf(" %s", argv[i]);
    }
    printf(" \n");
}

bool zetTranscodeMdl::parseCmd(void* cmd) {
    ZETCHECK_PTR_IS_NULL(cmd);
    const char     *opt;
    const char     *next;
    INT32 optindex = 1;
    bool err       = ZET_FALSE;

    if (cmdNum < 2) {
        LOG(LOG_ERROR, "the full input command info is incomplete, please check!!!");
        optionsExplain();
        return err;
    }

    char** inputInfo = reinterpret_cast<char**>(cmd);
    ZETCHECK_PTR_IS_NULL(*inputInfo);

    showTransCodeCmdInfo(cmdNum, inputInfo);

    const char* ffmpegPath = FFMPEG_BIN_PATH;
    char transCmdInfoBuffer[MAX_READING_LENGTH];
    snprintf(transCmdInfoBuffer, sizeof(transCmdInfoBuffer), "%s -y", ffmpegPath);

    char* curPtr = transCmdInfoBuffer + strlen(transCmdInfoBuffer);

    while (optindex < cmdNum) {
        opt  = (const char*)inputInfo[optindex++];
        if (optindex >= cmdNum) {
            optionsExplain();
            LOG(LOG_ERROR, "cmd line %s has no value, quit...\n", opt);
            goto PARSE_OPINIONS_OUT;
        }
        next = (const char*)inputInfo[optindex];

        if (!strcmp(opt, "-ab")) {
            transcodeInfo->audBitrate = atoi(next);
            if (transcodeInfo->audBitrate <= 0) {
                LOG(LOG_ERROR, "Invalid audio bitrate: %s", next);
                goto PARSE_OPINIONS_OUT;
            }

            appendOption(curPtr, "-b:a");
            *curPtr++ = ' ';
            curPtr += sprintf(curPtr, "%dk", transcodeInfo->audBitrate);
            optindex++;
            continue;
        }

        if(!strcmp(opt, "-hwaccels")) {
            appendOption(curPtr, "-hwaccels");
            appendOption(curPtr, next);
            optindex++;
            continue;
        }
		
        if (opt[0] == '-' ) {
            switch (opt[1]) {
                case 'i':
                    strncpy(transcodeInfo->file_input, next, MAX_FILE_NAME_LENGTH - 1);
                    transcodeInfo->file_input[MAX_FILE_NAME_LENGTH - 1] = '\0';
                    transcodeInfo->need_input = true;
                    // if (!strcmp(next, "http") || !strcmp(next, "rtmp") || !strcmp(next, "rtsp") || !strcmp(next, "m3u8")) {
                        transcodeInfo->lowLatency = true;
                        appendOption(curPtr, "-re");
                    // }
                    
                    appendOption(curPtr, "-i");
                    *curPtr++ = ' ';
                    curPtr += sprintf(curPtr, "%s", next);
                    optindex++;
                    break;
                case 'o':
                    strncpy(transcodeInfo->file_output, next, MAX_FILE_NAME_LENGTH - 1);
                    transcodeInfo->file_output[MAX_FILE_NAME_LENGTH - 1] = '\0';
                    transcodeInfo->need_output = true;
                    optindex++;
                    break;	   
                case 'b':
                    transcodeInfo->bitrate = atoi(next);
                    appendOption(curPtr, "-b:v");
                    *curPtr++ = ' ';
                    curPtr += sprintf(curPtr, "%dk", transcodeInfo->bitrate);	 
                    optindex++;
                    break;
                case 'w':
                    transcodeInfo->width = atoi(next);
                    optindex++;
                    break;
                case 'h':
            	    transcodeInfo->height = atoi(next);
                    if (transcodeInfo->height > 0) {
                        appendOption(curPtr, "-s");
                        *curPtr++ = ' ';
                        curPtr += sprintf(curPtr, "%dx%d", transcodeInfo->width, transcodeInfo->height);
                    }
                    if (transcodeInfo->width <= 0 || transcodeInfo->height <= 0) {
                        LOG(LOG_ERROR, "invalid input width or height, please check...");
                    }
                    optindex++;
                    break;
                case 'f':
                    transcodeInfo->framerate = atof(next);
                    appendOption(curPtr, "-r");
                    *curPtr++ = ' ';
                    curPtr += sprintf(curPtr, "%g", transcodeInfo->framerate);
                    optindex++;
                    break;
                case 's':
                    transcodeInfo->sampleRate = atoi(next);
                    appendOption(curPtr, "-ar");
                    *curPtr++ = ' ';
                    curPtr += sprintf(curPtr, "%d", transcodeInfo->sampleRate);
                    optindex++;
                    break;
                case 'n':
                    transcodeInfo->threads = atoi(next);
                    appendOption(curPtr, "-threads");
                    *curPtr++ = ' ';
                    curPtr += sprintf(curPtr, "%d", transcodeInfo->threads);
                    optindex++;
                    break;
                case 'c':
                    transcodeInfo->channels = atoi(next);
                    appendOption(curPtr, "-ac");
                    *curPtr++ = ' ';
                    curPtr += sprintf(curPtr, "%d", transcodeInfo->channels);
                    optindex++;
                    break;
                case 'a':
                    strncpy(transcodeInfo->audCodingType, next, MAX_TYPE_NAME_LENGTH - 1);
                    transcodeInfo->audCodingType[MAX_FILE_NAME_LENGTH - 1] = '\0';
                    appendOption(curPtr, "-c:a");
                    *curPtr++ = ' ';
                    curPtr += sprintf(curPtr, "%s", next);
                    optindex++;
                    break;
                case 'v':
                    strncpy(transcodeInfo->vidCodingType, next, MAX_TYPE_NAME_LENGTH - 1);
                    transcodeInfo->vidCodingType[MAX_FILE_NAME_LENGTH - 1] = '\0';
                    appendOption(curPtr, "-c:v");
                    *curPtr++ = ' ';
                    curPtr += sprintf(curPtr, "%s", next);
                    optindex++;
                    break;
                case 't':
                    strncpy(transcodeInfo->fileType, next, MAX_TYPE_NAME_LENGTH - 1);
                    transcodeInfo->fileType[MAX_FILE_NAME_LENGTH - 1] = '\0';
                    appendOption(curPtr, "-f");
                    *curPtr++ = ' ';
                    curPtr += sprintf(curPtr, "%s", next);
                    optindex++;
                    break;
                default:
                    LOG(LOG_ERROR, "unkonw parameters: %s", opt);
                    goto PARSE_OPINIONS_OUT;
            }
        } else {
            LOG(LOG_ERROR, "invalid parameters: %s, must start as '-', current transCmdInfoBuffer: %s", opt, transCmdInfoBuffer);
            goto PARSE_OPINIONS_OUT;
        }

    }

 {
    if (!transcodeInfo->need_input) {
        LOG(LOG_ERROR, "invalid input file, please specified the file");
        goto PARSE_OPINIONS_OUT;
    }

    if (!transcodeInfo->need_output && !transcodeInfo->lowLatency) {
        char* dotPos = strrchr(transcodeInfo->file_input, '.');
        int baseLen  = (dotPos != NULL) ? (dotPos - transcodeInfo->file_input) : strlen(transcodeInfo->file_input);
        snprintf(transcodeInfo->file_output, MAX_FILE_NAME_LENGTH,
                 "%.*s-output.%s", baseLen, transcodeInfo->file_input,
                 (transcodeInfo->fileType[0] ? transcodeInfo->fileType : "mp4"));
    }

    if (transcodeInfo->lowLatency) {

        if(!strcmp(transcodeInfo->vidCodingType, "libx264")) {
            appendOption(curPtr, "-preset ultrafast");
            appendOption(curPtr, "-tune film");
        }

        appendOption(curPtr, "-bf 2");

        appendOption(curPtr, "-muxdelay 0");

        appendOption(curPtr, "-muxpreload 0");

        appendOption(curPtr, "-flush_packets 0");

        appendOption(curPtr, "-use_wallclock_as_timestamps 0");

        appendOption(curPtr, "-f mpegts");

        appendOption(curPtr, "-fflags nobuffer -flags low_delay");

        //appendOption(curPtr, "-mpegts_flags resend_headers");

        appendOption(curPtr, "-force_key_frames");

        *curPtr++                     = ' ';
        const char* keyframe_settings = "\"expr:gte(n,n_forced*2)\" -g 60 -keyint_min 60";
        size_t remaining_space        = MAX_READING_LENGTH - (curPtr - transCmdInfoBuffer);
        if (strlen(keyframe_settings) < remaining_space) {
            strcpy(curPtr, keyframe_settings);
            curPtr += strlen(keyframe_settings);
        } else {
            LOG(LOG_ERROR, "transCmdInfoBuffer is too small to append keyframe_settings!");
            goto PARSE_OPINIONS_OUT;
        }

#if 1
/*
        char* dotPos = strrchr(transcodeInfo->file_input, '.');
        
        int baseLen  = (dotPos != NULL) ? (dotPos - transcodeInfo->file_input) : strlen(transcodeInfo->file_input);
       
        snprintf(transcodeInfo->file_output, MAX_FILE_NAME_LENGTH,
                "%.*s-output.%s", baseLen, transcodeInfo->file_input,
                (transcodeInfo->fileType[0] ? transcodeInfo->fileType : "ts"));

        *curPtr++ = ' ';

        strcpy(curPtr, transcodeInfo->file_output);

        curPtr += strlen(transcodeInfo->file_output);
*/

        const char* fullInputPath = transcodeInfo->file_input;
        const char* fileName	  = strrchr(fullInputPath, '/');
        //fileName == NULL ? fullInputPath : fileName++;

        if (fileName == NULL) {
            fileName = fullInputPath;
        } else {
            fileName++;
        }

        if (fileName == NULL || *fileName == '\0') {
            LOG(LOG_ERROR, "Invalid fileName extracted from path: %s", fullInputPath);
            return ZET_FALSE;
        }

        const char* dotPos        = strrchr(fileName, '.');
        INT32       baseLen 	  = (dotPos != NULL) ? (dotPos - fileName) : strlen(fileName);

        char baseName[MAX_FILE_NAME_LENGTH];
        snprintf(baseName, sizeof(baseName), "%.*s", baseLen, fileName);

        if (access(TRANSCODE_OUTPUT_PATH, F_OK) == -1) {
        	 mkdir(TRANSCODE_OUTPUT_PATH, 0755);
        }

        char transCodeSubDir [MAX_FILE_NAME_LENGTH];
//        snprintf (transCodeSubDir, sizeof (transCodeSubDir), "%s/%s-transcodeout", TRANSCODE_OUTPUT_PATH, baseName);

        INT32       len_output_path = strlen(TRANSCODE_OUTPUT_PATH);
        const char* suffix          = "-transcodeout";
        INT32       len_suffix      = strlen(suffix);
        INT32       max_base_len    = sizeof(transCodeSubDir) - len_output_path - 1 - len_suffix - 1;

        if (max_base_len <= 0) {
            LOG(LOG_ERROR, "TRANSCODE_OUTPUT_PATH is too long, cannot construct transCodeSubDir");
            return ZET_FALSE;
        }

        char truncated_base[MAX_FILE_NAME_LENGTH];
        strncpy(truncated_base, baseName, max_base_len);
        truncated_base[max_base_len] = '\0';
/*
        INT32 required_len = snprintf(NULL, 0, "%s/%s%s", TRANSCODE_OUTPUT_PATH, truncated_base, suffix) + 1;
        if (required_len > sizeof(transCodeSubDir)) {
            LOG(LOG_DEBUG, "Error: transCodeSubDir buffer too small. Required: %d, Actual: %d\n", required_len, (INT32)sizeof(transCodeSubDir));
            return ZET_FALSE;
        }
*/
        int result = snprintf(transCodeSubDir, sizeof(transCodeSubDir), "%s/%s%s", TRANSCODE_OUTPUT_PATH, truncated_base, suffix);

        if (result < 0) {
            LOG(LOG_ERROR, "snprintf failed to format transCodeSubDir");
            return ZET_FALSE;
        } else if (result >= (int)sizeof(transCodeSubDir)) {
            LOG(LOG_DEBUG, "transCodeSubDir was truncated. Required: %d, Actual: %d", result, (int)sizeof(transCodeSubDir));
        }

        LOG(LOG_DEBUG, "now the transCodeSubDir is: %s, baseName is: %s, fileName is : %s", transCodeSubDir, baseName, fileName);
       
        if (access(transCodeSubDir, F_OK) == 0) {
            char rmCmd[MAX_READING_LENGTH];
            if (chmod(transCodeSubDir, 0777) == -1) {
                LOG(LOG_ERROR, "Failed to set permissions for transCodeSubDir: %s, %s", strerror(errno), transCodeSubDir);
            }
            snprintf(rmCmd, sizeof(rmCmd), "rm -rf %s/*", transCodeSubDir);
            int  ret = system(rmCmd);
            if (ret != 0) {
                LOG(LOG_WARNING, "Failed to clear directory '%s', system() returned %d", transCodeSubDir, ret);
            } else {
                LOG(LOG_DEBUG, "clear the old data inside this file: %s", transCodeSubDir);
            }

        } else {
            LOG(LOG_DEBUG, "target file not exist, create it: %s", transCodeSubDir);
            mkdir(transCodeSubDir, 0777);
        }

        *curPtr++ = ' ';

//      snprintf(transcodeInfo->file_output, MAX_FILE_NAME_LENGTH, "%s/%s-output.%s", transCodeSubDir, baseName, (transcodeInfo->fileType[0] ? transcodeInfo->fileType : "ts"));

        const char* file_type           = transcodeInfo->fileType[0] ? transcodeInfo->fileType : "ts";
        INT32       len_file_type       = strlen(file_type);
        INT32       len_subdir          = strlen(transCodeSubDir);
        const char* outputSuffix        = "-output.";
        INT32       outputLen_suffix    = strlen(outputSuffix);
        INT32       outputMax_base_len  = MAX_FILE_NAME_LENGTH - len_subdir - 1 - outputLen_suffix - len_file_type - 1;

        if (outputMax_base_len <= 0) {
            LOG(LOG_ERROR, "transCodeSubDir or fileType is too long, cannot construct file_output");
            goto PARSE_OPINIONS_OUT;
        }

        char outputTruncated_base[MAX_FILE_NAME_LENGTH];
        strncpy(outputTruncated_base, baseName, outputMax_base_len);
        outputTruncated_base[outputMax_base_len] = '\0';

        INT32 required = snprintf(transcodeInfo->file_output, sizeof(transcodeInfo->file_output), "%s/%s%s",
                                  transCodeSubDir, baseName, (transcodeInfo->fileType[0] ? transcodeInfo->fileType : "ts"));

        if (required < 0) {
            LOG(LOG_ERROR, "snprintf failed to format transCodeSubDir");
            return ZET_FALSE;
        } else if (required >= (int)sizeof(transcodeInfo->file_output)) {
            LOG(LOG_DEBUG, "transCodeSubDir was truncated. Required: %d, Actual: %d", required, (int)sizeof(transcodeInfo->file_output));
        }

/*
        INT32 required = snprintf(NULL, 0, "%s/%s%s%s", transCodeSubDir, outputTruncated_base, outputSuffix, file_type) + 1;
        if (required > MAX_FILE_NAME_LENGTH) {
            LOG(LOG_DEBUG, "Error: file_output buffer too small. Required: %d, Actual: %d\n", required, MAX_FILE_NAME_LENGTH);
            return ZET_FALSE;
        }
        snprintf(transcodeInfo->file_output, MAX_FILE_NAME_LENGTH, "%s/%s%s%s", transCodeSubDir, outputTruncated_base, outputSuffix, file_type);
*/
        curPtr += snprintf(curPtr, transCmdInfoBuffer + sizeof(transCmdInfoBuffer) - curPtr,
        				 "%s", transcodeInfo->file_output);
#else
        appendOption(curPtr, "-fflags +nobuffer+discardcorrupt");

        appendOption(curPtr, "-max_delay 500000");

        appendOption(curPtr, "-x264-params keyint=25:min-keyint=12:force-cfr=1");

        appendOption(curPtr, "udp://127.0.0.1:1234?pkt_size=1316");
#endif
        if (currentLogLevel > LOG_DEBUG) {
            appendOption(curPtr, "-progress pipe:1");
            *curPtr++ = ' ';
            const char* grep_filter = "2>&1 | grep -E \"time|Opening|Initializing|Encoding\"";
            size_t remaining_space  = MAX_READING_LENGTH - (curPtr - transCmdInfoBuffer);
            if (strlen(grep_filter) < remaining_space) {
                strcpy(curPtr, grep_filter);
                curPtr += strlen(grep_filter);
            } else {
                LOG(LOG_ERROR, "transCmdInfoBuffer is too small to append grep filter!");
                goto PARSE_OPINIONS_OUT;
            }
        }
    }

    *curPtr = '\0';

    INT32 cmdLen = strlen(transCmdInfoBuffer) + 1;
    transCmdInfo = (char*)malloc(cmdLen);
    ZETCHECK_PTR_IS_NULL(transCmdInfo);

    strcpy(transCmdInfo, transCmdInfoBuffer);
    LOG(LOG_DEBUG, "output full ffmpeg command is: %s", transCmdInfo);
    err          = ZET_TRUE;
}
PARSE_OPINIONS_OUT:
	return err;
}

#if FFMPEG_EXE_WITH_CMDLINE
INT32 zetTranscodeMdl::processWithCmdLine(void* msg) {
    LOG(LOG_DEBUG, "called:");
    ZETCHECK_PTR_IS_NULL(transCmdInfo);

    char transFullCmd[MAX_READING_LENGTH];
#if USE_STATIC_LIB
    if(strlen(transCmdInfo) < MAX_READING_LENGTH) {
        snprintf(transFullCmd, sizeof(transFullCmd), "%s", transCmdInfo);
    } else {
        LOG(LOG_ERROR, "command info longer than default size, error");
        return ZET_NOK;
    }
#else
    int required_len = snprintf(NULL, 0, "export LD_LIBRARY_PATH=%s:$LD_LIBRARY_PATH && %s", FFMPEG_SHARED_LIB_PATH, transCmdInfo);
    if (required_len < 0 || required_len >= (int)sizeof(transFullCmd)) {
        LOG(LOG_ERROR, "transFullCmd buffer too small! Required: %d, Actual: %d", required_len, (int)sizeof(transFullCmd));
        return ZET_NOK;
    }
    snprintf(transFullCmd, sizeof(transFullCmd), "export LD_LIBRARY_PATH=%s:$LD_LIBRARY_PATH && %s", FFMPEG_SHARED_LIB_PATH, transCmdInfo);
#endif
    LOG(LOG_DEBUG, "full process command is: %s", transFullCmd);

    if (ffmpeg_pid != -1) {
    	kill(ffmpeg_pid, SIGTERM);
    	waitpid(ffmpeg_pid, NULL, WNOHANG);
    	ffmpeg_pid = -1;
    }

    pid_t pid = fork();
    if (pid == 0) {
    	execl("/bin/sh", "sh", "-c", transFullCmd, NULL);
    	LOG(LOG_ERROR, "execl failed: %s", strerror(errno));
    	exit(1);
    } else if (pid > 0) {
    	ffmpeg_pid = pid;
    	LOG(LOG_DEBUG, "FFmpeg process mark, ID: %d", ffmpeg_pid);
    	return ZET_OK;
    } else {
    	LOG(LOG_ERROR, "fork FFmpeg process failed");
    	return ZET_NOK;
    }
}
#else
INT32 zetTranscodeMdl::processWithApi(void* msg) {
    LOG(LOG_DEBUG, "called, current not support, please check...");
    return ZET_NOK;
}

INT32 zetTranscodeMdl::findBestStream(AVFormatContext* fmt_ctx, AVMediaType type, const std::string& type_name) {

    int stream_idx = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    
    if (stream_idx < 0) {
        LOG(LOG_ERROR, "do not find %s stream", type_name.c_str());
        return ZET_OK;
    }
    return stream_idx;
}

#endif

INT32 zetTranscodeMdl::process(void* msg) {
    INT32 ret = ZET_NOK;
#if FFMPEG_EXE_WITH_CMDLINE
    ret = this->processWithCmdLine(msg);
#else
    ret = this->processWithApi(msg); 
#endif
    return ret;
}

INT32 zetTranscodeMdl::processUpLayerCmd(const char* params, double seekTime) {
    ZETCHECK_PTR_IS_NULL(params);
    INT32 ret = ZET_NOK;
    stopFFmpegProcess();
    if (!strcasecmp(params, "seek")) {
        INT32 target_seekTime = (INT32)seekTime;
        LOG(LOG_DEBUG, " called, current seekTime: %d", target_seekTime);
        generateNewCmd(transCmdInfo, target_seekTime);
        ret = this->process(NULL);
        return ret;
    } else if (!strcasecmp(params, "stop")) {
        this->release();   
        LOG(LOG_DEBUG, " called, execute stop command");
        ret = ZET_OK;
    }
    return ret;
}

void zetTranscodeMdl::stopFFmpegProcess() {
#if FFMPEG_EXE_WITH_CMDLINE

    if (ffmpeg_pid == -1) return;

    if (ffmpeg_pid > 0) {
        kill(-ffmpeg_pid, SIGKILL);
        waitpid(ffmpeg_pid, NULL, 0);
        ffmpeg_pid = -1;
    }
#else

#endif
}

void zetTranscodeMdl::release() {
    SAFE_FREE(transCmdInfo);
    SAFE_FREE(transcodeInfo);
    // SAFE_DELETE(transcodeParam);
    // SAFE_DELETE(transcodeArgs);
}

zetTranscodeMdl::~zetTranscodeMdl() {
    this->release();
}

