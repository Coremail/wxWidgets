///////////////////////////////////////////////////////////////////////////////
// Name:        wx/unix/private/wakeuppipe.h
// Purpose:     Helper class allowing to wake up the main thread.
// Author:      Vadim Zeitlin
// Created:     2013-06-09 (extracted from src/unix/evtloopunix.cpp)
// Copyright:   (c) 2013 Vadim Zeitlin <vadim@wxwidgets.org>
// Licence:     wxWindows licence
///////////////////////////////////////////////////////////////////////////////

#ifndef _WX_UNIX_PRIVATE_WAKEUPPIPE_H_
#define _WX_UNIX_PRIVATE_WAKEUPPIPE_H_

#include "wx/unix/pipe.h"
#include "wx/evtloopsrc.h"

#include <atomic>

// ----------------------------------------------------------------------------
// wxWakeUpPipe: allows to wake up the event loop by writing to it
// ----------------------------------------------------------------------------

// This class is not MT-safe, see wxWakeUpPipeMT below for a wake up pipe
// usable from other threads.

class wxWakeUpPipe : public wxEventLoopSourceHandler
{
public:
    // Create and initialize the pipe.
    //
    // It's the callers responsibility to add the read end of this pipe,
    // returned by GetReadFd(), to the code blocking on input.
    wxWakeUpPipe();

    // Wake up the blocking operation involving this pipe.
    //
    // It simply writes to the write end of the pipe.
    //
    // As indicated by its name, this method does no locking and so can be
    // called only from the main thread.
    void WakeUpNoLock();

    // Same as WakeUp() but without locking.

    // Return the read end of the pipe.
    int GetReadFd() { return m_pipe[wxPipe::Read]; }


    // Implement wxEventLoopSourceHandler pure virtual methods
    virtual void OnReadWaiting() wxOVERRIDE;
    virtual void OnWriteWaiting() wxOVERRIDE { }
    virtual void OnExceptionWaiting() wxOVERRIDE { }

private:
    wxPipe m_pipe;

    // This flag is set to true after writing to the pipe and reset to false
    // after reading from it in the main thread. Having it allows us to avoid
    // overflowing the pipe with too many writes if the main thread can't keep
    // up with reading from it.
    //
    // It must be an atomic with "claim-before-write" protocol (see
    // WakeUpNoLock()): the signal handler can run on a secondary thread
    // (e.g. when SIGCHLD is delivered to a CEF/boost worker thread that
    // doesn't block it), truly concurrently with the main thread draining
    // the pipe in OnReadWaiting(). With the old non-atomic
    // "write-then-set-flag" order, the handler could be preempted between
    // the write() syscall and its flag store, letting the main thread drain
    // the byte and reset the flag meanwhile; the handler then stored "false"
    // (not empty) onto an EMPTY pipe, permanently wedging every future
    // wake-up (observed in the wild: xdg-open zombie processes).
    std::atomic<bool> m_pipeIsEmpty;
};

// ----------------------------------------------------------------------------
// wxWakeUpPipeMT: thread-safe version of wxWakeUpPipe
// ----------------------------------------------------------------------------

// This class can be used from multiple threads, i.e. its WakeUp() can be
// called concurrently.

class wxWakeUpPipeMT : public wxWakeUpPipe
{
#if wxUSE_THREADS
public:
    wxWakeUpPipeMT() { }

    // Thread-safe wrapper around WakeUpNoLock(): can be called from another
    // thread to wake up the main one.
    void WakeUp()
    {
        wxCriticalSectionLocker lock(m_pipeLock);

        WakeUpNoLock();
    }

    virtual void OnReadWaiting() wxOVERRIDE
    {
        wxCriticalSectionLocker lock(m_pipeLock);

        wxWakeUpPipe::OnReadWaiting();
    }

private:
    // Protects access to m_pipeIsEmpty.
    wxCriticalSection m_pipeLock;

#endif // wxUSE_THREADS
};

#endif // _WX_UNIX_PRIVATE_WAKEUPPIPE_H_
