#pragma once

#include <string>
#include <span>

#define __SHORT_FILE__ __FILE__
#if 1

#    undef __SHORT_FILE__
#    define __SHORT_FILE__   past_last_slash(__FILE__)
constexpr const char* past_last_slash(const char* const path, const int pos = 0,
                                      const int last_slash = 0) {
    if (path[pos] == '\0') return &path[last_slash];
    if (path[pos] == '/')
        return past_last_slash(path, pos + 1, pos + 1);
    else
        return past_last_slash(path, pos + 1, last_slash);
}

#endif

enum
{
    LOGLEVEL_TRACE = 0,
    LOGLEVEL_DEBUG = 1,
    LOGLEVEL_INFO  = 2,
    LOGLEVEL_WARN  = 3,
    LOGLEVEL_ERROR = 4,
};

// LOG_INFO has no file:line because it's used for high-volume status spam
// where the location is rarely interesting. The leveled variants (warn /
// error / debug / trace) carry __SHORT_FILE__:__LINE__ since those are the
// ones a reader follows back to the source.
#define LOG_TRACE(...) WallpaperLog(LOGLEVEL_TRACE, __SHORT_FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(...) WallpaperLog(LOGLEVEL_DEBUG, __SHORT_FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  WallpaperLog(LOGLEVEL_INFO,  "", 0, __VA_ARGS__)
#define LOG_WARN(...)  WallpaperLog(LOGLEVEL_WARN,  __SHORT_FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) WallpaperLog(LOGLEVEL_ERROR, __SHORT_FILE__, __LINE__, __VA_ARGS__)

// Runtime config is pulled from environment on first call:
//   WP_LOG_LEVEL=trace|debug|info|warn|error    (default: info)
//   WP_LOG_FILE=/path/to/log                    (default: stderr only; set to
//                                                tee to a file in addition)
//   WP_LOG_COLOR=0|1                            (default: auto from isatty)
void WallpaperLog(int level, const char* file, int line, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

std::string logToTmpfileWithSha1(std::span<const char>, const char* fmt, ...);
