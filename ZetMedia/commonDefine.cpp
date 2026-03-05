#include <fstream>

#include "commonDefine.h"

const char* FFMPEG_BIN_PATH        = NULL;
const char* FFMPEG_SHARED_LIB_PATH = NULL;

LogLevel currentLogLevel = (LogLevel)CURRENT_LOG_LEVEL;

bool is_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return ZET_FALSE;
    }
    return S_ISDIR(st.st_mode) == true ? ZET_TRUE: ZET_FALSE;
}

bool is_regular_file(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) 
        return ZET_FALSE;

    return S_ISREG(st.st_mode) == true ? ZET_TRUE: ZET_FALSE;
}

bool cache_file_exists() {
    struct stat buffer;
    return (stat(FFMPEG_PATH_CACHE_FILE, &buffer) == 0);
}

bool read_ffmpegPaths_from_cache() {
    std::ifstream cache_file(FFMPEG_PATH_CACHE_FILE);
    if (!cache_file) {
        LOG(LOG_ERROR, "Failed to open cache file for reading");
        return ZET_FALSE;
    }
 
    std::string line1, line2;
    if (!std::getline(cache_file, line1) || !std::getline(cache_file, line2)) {
        LOG(LOG_DEBUG, "Cache file is empty or malformed");
        return ZET_FALSE;
    }

    ffmpeg_bin_path      = line1;
    ffmpeg_shareLib_path = line2;

    bool ret = !ffmpeg_bin_path.empty() && !ffmpeg_shareLib_path.empty();
    if (!ret) {
        LOG(LOG_ERROR, "ffmpeg bin and lib path NULL, return...");
        return ZET_FALSE;
    }
	
#if USE_STATIC_LIB
	  FFMPEG_BIN_PATH	     = ffmpeg_bin_path.c_str();
	  LOG(LOG_DEBUG, "ffmpeg_bin_path: %s", FFMPEG_BIN_PATH);
#else
	  FFMPEG_BIN_PATH	     = ffmpeg_bin_path.c_str();
	  FFMPEG_SHARED_LIB_PATH = ffmpeg_shareLib_path.c_str();
	  LOG(LOG_DEBUG, "ffmpeg_bin_path: %s, ffmpeg_shareLib_path: %s", FFMPEG_BIN_PATH, FFMPEG_SHARED_LIB_PATH);
#endif
    return ret;
}

void write_paths_to_cache(const std::string& bin_path, const std::string& lib_path) {
    std::ofstream cache_file(FFMPEG_PATH_CACHE_FILE);
    if (!cache_file) {
        LOG(LOG_ERROR, "Failed to open cache file for writing: %s", strerror(errno));
        return;
    }

    if (access(FFMPEG_PATH_CACHE_FILE, F_OK) == -1) {
        LOG(LOG_ERROR, "Cache file does not exist: %s", strerror(errno));
        std::ofstream file(FFMPEG_PATH_CACHE_FILE);
        if (!file) {
            LOG(LOG_ERROR, "Failed to create cache file: %s", strerror(errno));
        }
        file.close();
        if (chmod(FFMPEG_PATH_CACHE_FILE, 0755) == -1) {
            LOG(LOG_ERROR, "Failed to set permissions: %s", strerror(errno));
        } else {
            LOG(LOG_DEBUG, "Cache file created and permissions set");
        }
    } else if (access(FFMPEG_PATH_CACHE_FILE, W_OK) == -1) {
            LOG(LOG_ERROR, "Cache file exists but is not writable: %s", strerror(errno));
        if (chmod(FFMPEG_PATH_CACHE_FILE, 0755) == -1) {
            LOG(LOG_ERROR, "Failed to fix permissions: %s", strerror(errno));
        }
    }
    cache_file << bin_path << std::endl << lib_path<< std::endl;
    LOG(LOG_DEBUG, "Cache written: %s, %s", bin_path.c_str(), lib_path.c_str());
}

std::string path_join(const std::string& dir, const std::string& filename) {
    if (dir.empty()) {
        return filename;
    }

    char last_char = dir[dir.size() - 1];
    if (last_char == '/') {
        return dir + filename;
    } else {
        return dir + "/" + filename;
    }
}

bool search_file_recursive(const std::string& root_dir, const std::string& target_filename, std::string& result_path) {

    DIR* dir = opendir(root_dir.c_str());
    if (dir == NULL) {
        return ZET_FALSE;
    }

    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        std::string entry_name = entry->d_name;
        std::string entry_path = path_join(root_dir, entry_name);

        if (is_directory(entry_path)) {
            if (search_file_recursive(entry_path, target_filename, result_path)) {
                closedir(dir);
                return ZET_TRUE;
            }
        } 

        else if (is_regular_file(entry_path)) {
            if (entry_name == target_filename) {
                result_path = entry_path;
                closedir(dir);
                return ZET_TRUE;
            }
        }
    }
    closedir(dir);
    return ZET_FALSE;
}

const char** get_ffmpeg_search_roots(int* count) { LOG(LOG_ERROR, "list ffmepg tools lib...");

    static const char* roots[10];
    int index      = 0;
    roots[index++] = "/usr/lib";
    roots[index++] = "../";
    roots[index++] = "/usr/bin";
    roots[index++] = "/usr/local/bin";
    roots[index++] = "/usr/local/lib";

    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        roots[index++] = cwd;
    }
    *count = index;
    return roots;
}

std::string find_ffmpeg_executable() {
    std::string  target_filename = "ffmpeg";
    INT32        pathCount       = 0;
    const char** paths           = get_ffmpeg_search_roots(&pathCount);
    std::string result_path;

    for (INT32 i = 0; i < pathCount; ++i) {
        if (search_file_recursive(paths[i], target_filename, result_path)) {
            return result_path;
        }
    }
    return "";
}

std::string find_ffmpeg_library_dir() {
    std::string target_libname;
    target_libname          = "libavcodec.so";
    INT32        pathCount  = 0;
    const char** paths      = get_ffmpeg_search_roots(&pathCount);
    std::string  lib_full_path;

    for (size_t i = 0; i < pathCount; ++i) {
        if (search_file_recursive(paths[i], target_libname, lib_full_path)) {
            const char* last_slash = strrchr(lib_full_path.c_str(), '/');
            if (last_slash == NULL) {
                return lib_full_path;
            }
            return lib_full_path.substr(0, last_slash - lib_full_path.c_str());
        }
    }
    return "";
}

void optionsExplain() {
    int optionSum = sizeof(zet_cmd_list)/sizeof(optionInfo);
    for (int i = 0; i < optionSum; i++) {
        LOG(LOG_DEBUG, "-%s  %-16s\t%s\n", zet_cmd_list[i].shortname, zet_cmd_list[i].fullName, zet_cmd_list[i].helpInfo);
    }
}

void appendOption(char* &ptr, const char* opt) {
    *ptr++ = ' ';
    strcpy(ptr, opt);
    ptr += strlen(opt);
}

void generateNewCmd(char* info, int target_seekTime) {
    if (!info) {
        LOG(LOG_DEBUG, " null ptr found, return dierectly");
        return;
    }

    if (strlen(info) >= MAX_READING_LENGTH - 10) { // ensure to have enough space to insert new command
        LOG(LOG_ERROR, "Command too long to insert seek time");
        return;
    }

    const char* insertPoint = " -i";
    char* pos = strstr(info, insertPoint);
    if (!pos) {
        LOG(LOG_ERROR, "Failed to find insertion point ' -i'");
        return;
    }

    INT32 offset = pos - info;
    char newCmd[MAX_READING_LENGTH] = {0};
    snprintf(newCmd, offset + 1, "%s", info);
    snprintf(newCmd + offset, sizeof(newCmd) - offset, 
             " -ss %d %s", target_seekTime, pos);
    strcpy(info, newCmd);
}

