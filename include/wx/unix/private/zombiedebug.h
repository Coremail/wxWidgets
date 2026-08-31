/////////////////////////////////////////////////////////////////////////////
// Name:        wx/unix/private/zombiedebug.h
// Purpose:     Temporary debug helpers for diagnosing zombie process bug.
//              [zombie-debug] Provides async-signal-safe logging for use
//              inside signal handlers, plus a convenience macro for the
//              timestamped wxLog-based logging used in normal context.
// Author:      coremail
// Created:     2026-08-28
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////

#ifndef _WX_UNIX_PRIVATE_ZOMBIEDEBUG_H_
#define _WX_UNIX_PRIVATE_ZOMBIEDEBUG_H_

#include "wx/defs.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

// ----------------------------------------------------------------------------
// [zombie-debug] Async-signal-safe logging for signal handler context.
//
// std::ofstream / wxLog are NOT async-signal-safe and can deadlock if the
// signal interrupts malloc or stdio while they hold a lock.  We use
// open(2)/write(2)/close(2) which are in the POSIX async-signal-safe list.
//
// NOTE: only short literal strings and small integers can be logged.
// NOTE: this section uses no wx API at all, so it can be included from
//       translation units built without wxUSE_LOG/wxUSE_STREAMS.
// ----------------------------------------------------------------------------

#define WX_ZOMBIE_DEBUG_LOG_FILE "/tmp/wx_execute_log"

// Writes msg (a string literal) followed by '\n' to the debug log.
// Async-signal-safe.  Returns the fd used (or -1 on failure).
inline int wxZombieDbgLogSigSafe(const char* msg)
{
    int fd = open(WX_ZOMBIE_DEBUG_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if ( fd < 0 )
        return -1;

    // compute length without strlen() (not guaranteed async-signal-safe)
    size_t len = 0;
    while ( msg[len] ) ++len;

    (void)write(fd, msg, len);
    (void)write(fd, "\n", 1);
    (void)close(fd);
    return fd;
}

// Same as above but appends a small integer (e.g. an fd or errno value)
// after the message.  Async-signal-safe.
inline int wxZombieDbgLogSigSafeInt(const char* msg, int value)
{
    int fd = open(WX_ZOMBIE_DEBUG_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if ( fd < 0 )
        return -1;

    size_t len = 0;
    while ( msg[len] ) ++len;
    (void)write(fd, msg, len);

    // manually format the integer (avoid snprintf, not async-signal-safe)
    char buf[16];
    int n = 0;
    unsigned int v = (value < 0) ? (unsigned int)(-(long)value) : (unsigned int)value;
    if ( value < 0 )
    {
        char c = '-';
        (void)write(fd, &c, 1);
    }
    do
    {
        buf[n++] = '0' + (v % 10);
        v /= 10;
    } while ( v && n < (int)sizeof(buf) );
    while ( n > 0 )
    {
        char c = buf[--n];
        (void)write(fd, &c, 1);
    }

    (void)write(fd, "\n", 1);
    (void)close(fd);
    return fd;
}

// ----------------------------------------------------------------------------
// [zombie-debug] Normal-context (wxLog-based) logging helper.  NOT for use
// inside signal handlers.
//
// Behavior-preserving guarantees:
//  1. The previously active log target is SAVED and RESTORED, so the
//     application's own log target is not detached (calling
//     SetActiveTarget(nullptr) would leave the target null and the next
//     wxLogXXX from app code would auto-create a default one - a real
//     behavior change).
//  2. The formatted string is passed as %s to wxVLogInfo, never as the
//     format string, so '%' in logged content (e.g. file paths) cannot
//     confuse wx's printf parser.
// ----------------------------------------------------------------------------

#if wxUSE_LOG && wxUSE_STD_IOSTREAM

#include "wx/log.h"

#include <cstdarg>
#include <fstream>

inline void wxZombieDbgLog(const char* fmt, ...)
{
    // Format completely FIRST, with the app's logger untouched.
    va_list argptr;
    va_start(argptr, fmt);
    const wxString msg = wxString::FormatV(fmt, argptr);
    va_end(argptr);

    (void) std::ofstream(WX_ZOMBIE_DEBUG_LOG_FILE, std::ios::app);
    std::fstream zlog(WX_ZOMBIE_DEBUG_LOG_FILE,
                      std::ios::in | std::ios::out | std::ios::app);
    if ( !zlog.is_open() )
        return; // [zombie-debug] never let debug logging alter control flow

    wxLogStream logger_(&zlog);
    wxLog* oldTarget = wxLog::SetActiveTarget(&logger_);
    // Format is a literal: msg is strictly data, never parsed for '%'
    // (fixes the format-string hazard of logging raw paths as formats).
    wxLogInfo(wxS("%s"), msg);
    wxLog::SetActiveTarget(oldTarget);   // restore app's target (see note 1)
}

#endif // wxUSE_LOG && wxUSE_STD_IOSTREAM

#endif // _WX_UNIX_PRIVATE_ZOMBIEDEBUG_H_
