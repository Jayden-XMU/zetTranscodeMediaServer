#include <iostream>
#include <fstream>
#include "zetBusFactory.h"
#include "zetFeatureServerMdl.h"
#include "zetTranscodeMdl.h"
#include "zetCtlMdl.h"
#include "zetHlsServerMdl.h"
#include "iZetBusFactory.h"
#include "commonDefine.h"
#include <atomic>
#include <vector>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/types.h>

using namespace std;
static const char* testCommand[] = {
  "zetTranscodeMdl",
  "zetHlsServerMdl",
  "zetFeatureServerMdl",
};

INT32 server_fd                          = -1;
INT32 sleep_interval                     = 0;
static  char*  UNIX_SOCKET_PATH          = "/tmp/zetMediaServer_ipc.sock";
static std::atomic<bool> need_exit{ZET_FALSE};
static std::vector<int> client_fds;
static pthread_mutex_t  client_fds_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t  exit_mutex       = PTHREAD_MUTEX_INITIALIZER;
static bool             start_to_process = ZET_FALSE;

typedef struct _threadInfo {
    int        client_fd; 
    zetCtlMdl* ctrlMdl;
} threadInfo;

void signal_handler(int sig) {
    (void)sig;
    need_exit.store(ZET_TRUE, std::memory_order_relaxed);
}

// handle the seek command from frontned, running in child thread in case of blocking the main thread
void* handleIPCClient(void* args) {
    LOG(LOG_DEBUG, "the child thread create now.");

    threadInfo* tInfo = static_cast<threadInfo*>(args);
    ZETCHECK_PTR_IS_NULL(tInfo);

    INT32 client_fd   = tInfo->client_fd;
    zetCtlMdl* ctlMdl = tInfo->ctrlMdl;
    SAFE_DELETE(tInfo);

    if (!ctlMdl) {
        LOG(LOG_ERROR, "can not create message interaction obj, so stop this.");
        return NULL;
    }

    zetHlsServerMdl*hlsSrvMdl = dynamic_cast<zetHlsServerMdl*>(ctlMdl ->getCtlObj(ZETHLSSERVERMODULE));

    if (!hlsSrvMdl) {
        LOG(LOG_ERROR, "get hlsSrvMdl failed");
        return NULL;
    }

    pthread_mutex_lock(&client_fds_mutex);
    client_fds.push_back(client_fd);
    pthread_mutex_unlock(&client_fds_mutex);

    char buf[MAX_READING_LENGTH] = {0};

    hlsMsgType preMsgType = Transcode_unInitialed;

    while (!need_exit.load(std::memory_order_relaxed)) {
        // read the command from frontend, format: seek 60,
#if 0
        int n = read(client_fd, buf, sizeof(buf)-1);
        if (n <= 0) {
            LOG(LOG_DEBUG, "clinet side disconnect");
            break;
        }
        buf[n] = '\0';
        LOG(LOG_DEBUG, "receive IPC command now: %s", buf);

        std::string cmd(buf);
        if (cmd.find("seek") != std::string::npos) {
            INT32 seek_pos     = cmd.find("seek");
            double target_time = std::stod(cmd.substr(seek_pos + 4));
            LOG(LOG_DEBUG, "process Seek cmd, target time: %.2f second", target_time);
            ctlMdl->processUpLayerCmd("seek", target_time);
            const char* resp = "seek_ok";
            ssize_t written  = write(client_fd, resp, strlen(resp));
            if (written < 0) {
                if (errno == EINTR) {  // signal interruptted, try again
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {  //non-blocked IO
                    break;
                } else {
                    perror("write failed");
                    return NULL;
                }
            } else if (written == 0) {  // remote side connection closed
                break;
            }
        } else if (cmd.find("stop") != std::string::npos) {
            LOG(LOG_DEBUG, "receive stop cmd and need to stop now");
            ctlMdl->processUpLayerCmd("stop", 0);
            pthread_mutex_lock(&exit_mutex);
            need_exit = ZET_TRUE;
            pthread_mutex_unlock(&exit_mutex);
            break;
        }
#endif 
        hlsMsgType msgType = hlsSrvMdl->getHLSMsgType();
        if (msgType == preMsgType) {
            usleep(sleep_interval * 1000);
            continue;
        }
        preMsgType = msgType;

        if (msgType != Transcode_unInitialed) {
            char resp[128] = {'0'};
            switch (msgType) {
              case Transcode_error:
                strcpy(resp, "Transcode_error");
                LOG(LOG_INFO, "Transcode_error message generated");
                break;
              case Transcode_start:
                strcpy(resp, "Transcode_start");
                LOG(LOG_INFO, "Transcode_start message generated");
                break;
/*
              case Transcode_working:
                strcpy(resp, "Transcode_working");
                LOG(LOG_INFO, "Transcode_working message generated");
                break;
*/
              case Transcode_finished:
                strcpy(resp, "Transcode_finished");
                LOG(LOG_INFO, "Transcode_finished message generated");
                break;
              case Transcode_lastSegment_generated:
                strcpy(resp, "Transcode_lastSegment_generated");
                LOG(LOG_INFO, "Transcode_lastSegment_generated message generated");
                break;
              default:
                break;
          }
         size_t written = write(client_fd, resp, strlen(resp));
         if (written < 0) {
             if (errno == EINTR) {
                 continue;
             } else if (errno == EAGAIN || errno == EWOULDBLOCK) {  //non-blocked IO
                 break;
             } else {
                 LOG(LOG_ERROR, "socket server write msg error...");
                 return NULL;
             }
         } else if (written == 0) {  // remote side connection closed
             break;
         }
        // hlsSrvMdl->resetHLSMsg();
        }
    }

    if (need_exit.load(std::memory_order_relaxed)) {
        LOG(LOG_DEBUG, "the server is ready to exit, stop to listen");
    }

    // Remove this fd from global tracking list before closing to avoid
    // double-closing or closing a reused descriptor.
    pthread_mutex_lock(&client_fds_mutex);
    for (size_t i = 0; i < client_fds.size(); ++i) {
        if (client_fds[i] == client_fd) {
            client_fds[i] = -1;
        }
    }
    pthread_mutex_unlock(&client_fds_mutex);

    close(client_fd);
    return NULL;
}

void* transcodeThread(void* arg) {
    zetCtlMdl* ctlMdl = static_cast<zetCtlMdl*>(arg);
    ZETCHECK_PTR_IS_NULL(ctlMdl);
    INT32 ret         = ctlMdl->process(NULL);
    if (ret != ZET_OK) {
        LOG(LOG_ERROR, "FFmpeg process failed with code: %d, need to exit", ret);
    } else {
        LOG(LOG_DEBUG, "FFmpeg process successfully with code: %d, need to exit", ret);
    }

    // Signal all IPC threads to exit. Socket descriptors will be
    // closed either by the IPC thread itself or by the main thread
    // as a safety net after all threads have finished.
    need_exit.store(ZET_TRUE, std::memory_order_relaxed);
    return NULL;
}

void init_log_level() { 

    const char* env_log_level = getenv("ZET_LOG_LEVEL");

    LogLevel initial_level = (LogLevel)CURRENT_LOG_LEVEL;
    if (currentLogLevel != initial_level) {
        currentLogLevel = initial_level;
        LOG(LOG_DEBUG, "Set currentLogLevel to compile-time default: %d", CURRENT_LOG_LEVEL);
    }
    if (!env_log_level) {
        LOG(LOG_INFO, "No LOG_LEVEL env set, using compile-time default: %d", CURRENT_LOG_LEVEL);
        return;
    }

    LogLevel new_level = initial_level;

    if (strcmp(env_log_level, "VERBOSE") == 0 || strcmp(env_log_level, "0") == 0) {
        new_level = LOG_VERBOSE;
    } else if (strcmp(env_log_level, "DEBUG") == 0 || strcmp(env_log_level, "1") == 0) {
        new_level = LOG_DEBUG;
    } else if (strcmp(env_log_level, "INFO") == 0 || strcmp(env_log_level, "2") == 0) {
        new_level = LOG_INFO;
    } else if (strcmp(env_log_level, "WARNING") == 0 || strcmp(env_log_level, "3") == 0) {
        new_level = LOG_WARNING;
    } else if (strcmp(env_log_level, "ERROR") == 0 || strcmp(env_log_level, "4") == 0) {
        new_level = LOG_ERROR;
    } else if (strcmp(env_log_level, "CRITICAL") == 0 || strcmp(env_log_level, "5") == 0) {
        new_level = LOG_CRITICAL;
    } else {
        LOG(LOG_WARNING, "Invalid LOG_LEVEL env: %s, use compile-time default: %d", env_log_level, CURRENT_LOG_LEVEL);
        new_level = initial_level;
    }

    currentLogLevel = new_level;
    LOG(LOG_INFO, "Log level set by env: %s (enum value: %d)", env_log_level, currentLogLevel);
}

int getTargetHbi(int argc, char* argv[]) {
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-hbi") == 0) {
            int hbi_value = atoi(argv[i + 1]);
            LOG(LOG_DEBUG, "Found hbi parameter: %d", hbi_value);
            return hbi_value;
        }
    }
    return -1;
}

int main(int argc, char* argv[]) {
    init_log_level();
    // step 1: find target ffmpeg file using cache
    bool use_cache = ZET_FALSE;
    LOG(LOG_DEBUG, "Cache file path: %s", FFMPEG_PATH_CACHE_FILE);

    if (cache_file_exists()) {
        use_cache = read_ffmpegPaths_from_cache();
        LOG(LOG_DEBUG, "cache file exist, and use_cache: %d", use_cache);
    }

    if (!use_cache) {
        LOG(LOG_ERROR, "need to find ffmpeg path first.");
#if USE_STATIC_LIB
        ffmpeg_bin_path        = find_ffmpeg_executable();
        FFMPEG_BIN_PATH        = ffmpeg_bin_path.c_str();
        LOG(LOG_ERROR, "ffmpeg_bin_path: %s", ffmpeg_bin_path.c_str());
#else
        ffmpeg_bin_path        = find_ffmpeg_executable();
        ffmpeg_shareLib_path   = find_ffmpeg_library_dir();
        FFMPEG_BIN_PATH        = ffmpeg_bin_path.c_str();
        FFMPEG_SHARED_LIB_PATH = ffmpeg_shareLib_path.c_str();
        LOG(LOG_ERROR, "ffmpeg_bin_path: %s, ffmpeg_shareLib_path: %s", ffmpeg_bin_path.c_str(), ffmpeg_shareLib_path.c_str());
#endif
        write_paths_to_cache(ffmpeg_bin_path, ffmpeg_shareLib_path);
    }
    // step 2: register processing module and start processing
    iZetBusFactory* zetBusFty     = new zetBusFactory;
    iZetModule*     zetCtlModule  = zetBusFty->createZetModule("zetControlModule"); // create base Module first
    zetCtlMdl*      ctlMdl        = NULL;
    bool            err           = ZET_FALSE;
    if (zetCtlModule) {
        ctlMdl = dynamic_cast<zetCtlMdl*>(zetCtlModule);
    } else {
        LOG(LOG_ERROR, "error in create base module, return");
        SAFE_DELETE(zetBusFty);
        return ZET_NOK;
    }

    for (int i = 0; i < sizeof(testCommand)/sizeof(char*); i++) {
        const char* moduleName = const_cast<const char*>(testCommand[i]);
        iZetModule* module     = zetBusFty->createZetModule(moduleName);
        if (!module) {
            LOG(LOG_ERROR, "error in create module, please check!!!");
            goto ERROR_PROCESS;
        }

        if (!strcmp(testCommand[i], "zetTranscodeMdl")) {
            err = ctlMdl->registerCtlObj(module, ZETTRANSCODEMODULE);
        } else if (!strcmp(testCommand[i], "zetHlsServerMdl")) {
            err = ctlMdl->registerCtlObj(module, ZETHLSSERVERMODULE);
        } else if (!strcmp(testCommand[i], "zetFeatureServerMdl")) {
            err = ctlMdl->registerCtlObj(module, ZETFEATURESERVERMODULE);
        }
        if (err != ZET_TRUE) {
            LOG(LOG_ERROR, "error in register module, return!!!");
            goto ERROR_PROCESS;
        }
    }

    pthread_t mediaProcess_id;
    pthread_t ipcSocket_id;
    bool      ipc_thread_created;
    ipc_thread_created = ZET_FALSE;
    sleep_interval = getTargetHbi(argc, argv);
    if (sleep_interval == 0) {
        LOG(LOG_ERROR, "could not get sleep interval properly, please check!!!");
    } else {
         LOG(LOG_ERROR, "get sleep interval time: %d", sleep_interval);
	}
    if (ctlMdl->preParseCmd(argc, argv) != ZET_OK) {
        LOG(LOG_ERROR, "preParseCmd error, please check!!!");
        goto ERROR_PROCESS;
    } else {
        if (pthread_create(&mediaProcess_id, NULL, transcodeThread, ctlMdl) !=0) {
            LOG(LOG_ERROR, "Failed to start transcode thread");
            goto ERROR_PROCESS;
        }
    }

    // create listen thread to communicate with frontend 
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
	int   client_fd;
	client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd < 0) {
        LOG(LOG_ERROR, "failed to create unix socket, client fd=%d", client_fd);
    } else {
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, UNIX_SOCKET_PATH, sizeof(address.sun_path) - 1);
        if (connect(client_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            close(client_fd);
            client_fd = -1;
            LOG(LOG_ERROR, "Failed to connect to server");
        } else {
            threadInfo* tInfo = new threadInfo;
            tInfo->client_fd  = client_fd;
            tInfo->ctrlMdl    = ctlMdl;
            int ret = pthread_create(&ipcSocket_id, NULL, handleIPCClient, tInfo);
            if (ret != 0) {
                LOG(LOG_ERROR, "faile to create handleIPC thread, fd: %d, error number: %d", client_fd, ret);
                close(client_fd);
                client_fd = -1;
                SAFE_DELETE(tInfo);
            } else {
                ipc_thread_created = ZET_TRUE;
                LOG(LOG_DEBUG, "Thread created for fd: %d", client_fd);
            }
        }
    }
    LOG(LOG_DEBUG, "ready to release resource here");
    pthread_join(mediaProcess_id, NULL);
    if (ipc_thread_created) {
        pthread_join(ipcSocket_id, NULL);
    }

    // As a safety net, close any remaining client fds that might not
    // have been closed by IPC threads (e.g., on error paths).
    pthread_mutex_lock(&client_fds_mutex);
    for (size_t i = 0; i < client_fds.size(); ++i) {
        if (client_fds[i] != -1) {
            shutdown(client_fds[i], SHUT_RDWR);
            close(client_fds[i]);
            client_fds[i] = -1;
        }
    }
    client_fds.clear();
    pthread_mutex_unlock(&client_fds_mutex);
    pthread_mutex_destroy(&client_fds_mutex);
    pthread_mutex_destroy(&exit_mutex);

    if (ctlMdl) {
        ctlMdl->release();
    }
    SAFE_DELETE(ctlMdl);
    SAFE_DELETE(zetBusFty);
    return ZET_OK;

ERROR_PROCESS:
    if (ctlMdl) {
        ctlMdl->release();
    }
    SAFE_DELETE(ctlMdl);
    SAFE_DELETE(zetBusFty);
    return ZET_NOK;
}
