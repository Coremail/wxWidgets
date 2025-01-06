#include "wx/gtk/private/debughlp.h"

bool wxGtkDebugLog(const char* format, const char* function_name, int line_num, const char* src_file, ...){
    const static auto GDK_DEBUG_defined = wxGetEnv("GDK_DEBUG", nullptr);
    if (!GDK_DEBUG_defined)
        return false;
    ::FILE* logfile_ = std::fopen("/tmp/cmclient_gtk.log", "a");
    timespec nano_now;
    clock_gettime(CLOCK_REALTIME, &nano_now);
    if (logfile_ != nullptr){
        va_list argptr;
        va_start(argptr, src_file);
        fprintf(logfile_, "PreciseTime:%ld,%ld. %s:%d:%s--->", nano_now.tv_sec, nano_now.tv_nsec, src_file, line_num, function_name);
        vfprintf(logfile_, format, argptr);
        va_end(argptr);
        std::fclose(logfile_);
        return true;
    }
    return false;
}