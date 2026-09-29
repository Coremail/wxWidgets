///////////////////////////////////////////////////////////////////////////////
// Name:        src/unix/wakeuppipe.cpp
// Purpose:     Implementation of wxWakeUpPipe class.
// Author:      Vadim Zeitlin
// Created:     2013-06-09 (extracted from src/unix/evtloopunix.cpp)
// Copyright:   (c) 2013 Vadim Zeitlin <vadim@wxwidgets.org>
// Licence:     wxWindows licence
///////////////////////////////////////////////////////////////////////////////

// ============================================================================
// declarations
// ============================================================================

// ----------------------------------------------------------------------------
// headers
// ----------------------------------------------------------------------------

// for compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"


#ifndef WX_PRECOMP
#endif // WX_PRECOMP

#include "wx/unix/private/wakeuppipe.h"

// [zombie-debug] shared async-signal-safe logging helpers
#include "wx/unix/private/zombiedebug.h"

#include <errno.h>

// ----------------------------------------------------------------------------
// constants
// ----------------------------------------------------------------------------

#define TRACE_EVENTS wxT("events")

// ============================================================================
// wxWakeUpPipe implementation
// ============================================================================

// ----------------------------------------------------------------------------
// initialization
// ----------------------------------------------------------------------------

wxWakeUpPipe::wxWakeUpPipe()
{
    m_pipeIsEmpty = true;

    if ( !m_pipe.Create() )
    {
        wxLogError(_("Failed to create wake up pipe used by event loop."));
        return;
    }


    if ( !m_pipe.MakeNonBlocking(wxPipe::Read) )
    {
        wxLogSysError(_("Failed to switch wake up pipe to non-blocking mode"));
        return;
    }

    wxLogTrace(TRACE_EVENTS, wxT("Wake up pipe (%d, %d) created"),
               m_pipe[wxPipe::Read], m_pipe[wxPipe::Write]);
}

// ----------------------------------------------------------------------------
// wakeup handling
// ----------------------------------------------------------------------------

void wxWakeUpPipe::WakeUpNoLock()
{
    // [zombie-debug] Log every wake-up attempt and its outcome.
    //
    // This runs in SIGNAL HANDLER context (from wxAppConsole::HandleSignal),
    // so we must use the async-signal-safe logger only.
    //
    // Note: the signal handler may run on a SECONDARY thread (SIGCHLD is
    // process-directed and is delivered to any thread not blocking it, e.g.
    // CEF/boost worker threads), so this can execute truly concurrently with
    // OnReadWaiting() on the main thread. The claim-before-write exchange
    // below is what makes that safe (see the comment in wakeuppipe.h).

    // Atomically claim the "pipe not empty" state BEFORE writing, by
    // exchanging the flag to false ("not empty"). The exchange returns the
    // previous value: if it was true ("empty"), we won the claim and must
    // perform the write; if it was false, a wake-up byte is already pending
    // (or being written by another signal handler) and we can skip.
    //
    // This claim-before-write ordering is what makes the concurrent signal
    // handler (which may run on a secondary thread, see the header comment)
    // safe: the flag can never be left at "not empty" while the pipe is
    // actually empty. In the worst interleaving the main thread's drain
    // resets the flag while our byte has not been written yet - then our
    // byte simply lands in an already-"empty" pipe, costing at most one
    // stale byte and one extra spurious wake-up later (harmless, the drain
    // loop handles it), but never a permanently lost wake-up.
    if ( !m_pipeIsEmpty.exchange(false) )
    {
        // [zombie-debug] pipe already claimed non-empty: a wake-up byte is
        // pending (or being written), nothing to do.
        wxZombieDbgLogSigSafe("WakeUpNoLock: SKIP write, pipe not empty (previous byte never read by event loop)");
        return;
    }

    const int wfd = m_pipe[wxPipe::Write];
    if ( write(wfd, "s", 1) != 1 )
    {
        // [zombie-debug] Preserve errno before calling any logging function.
        const int savedErrno = errno;

        // The write failed, so undo our claim ("empty" again) to let a later
        // call retry.
        m_pipeIsEmpty.store(true);

        // don't use wxLog here, we can be in another thread and this could
        // result in dead locks
        perror("write(wake up pipe)");

        // [zombie-debug] write failed: EBADF means the write fd was closed
        // by external code (fd table corruption) - a direct cause of
        // permanently lost SIGCHLD wake-ups.
        if ( savedErrno == EBADF )
            wxZombieDbgLogSigSafeInt("WakeUpNoLock: write FAILED with EBADF (write fd closed!), fd=", wfd);
        else
            wxZombieDbgLogSigSafeInt("WakeUpNoLock: write FAILED, fd=", wfd);
        wxZombieDbgLogSigSafeInt("WakeUpNoLock: ... errno=", savedErrno);
    }
    else
    {
        // We just wrote to it, so it's not empty any more - and this was
        // already recorded by our claim above.
        wxZombieDbgLogSigSafe("WakeUpNoLock: wrote wake-up byte OK");
    }
}

void wxWakeUpPipe::OnReadWaiting()
{
    // got wakeup from child thread, remove the data that provoked it from the
    // pipe

    // [zombie-debug] Log the read fd: this function is only called when the
    // event loop watch fires.  If the fd number was reused by unrelated code
    // (original pipe fd closed externally), we'll see it here.
    int totalDrained = 0;

    char buf[4];
    for ( ;; )
    {
        const int size = read(GetReadFd(), buf, WXSIZEOF(buf));

        if ( size > 0 )
        {
            wxASSERT_MSG( size == 1, "Too many writes to wake-up pipe?" );
            totalDrained += size;

            break;
        }

        if ( size == 0 || (size == -1 && errno == EAGAIN) )
        {
            // No data available, not an error (but still surprising,
            // spurious wakeup?)
            break;
        }

        if ( errno == EINTR )
        {
            // We were interrupted, try again.
            continue;
        }

        // [zombie-debug] read() hard failure (e.g. EBADF: fd closed).
        // The wake-up pipe is dead - all future SIGCHLD wake-ups will be
        // lost and children will become zombies.
        wxZombieDbgLogSigSafeInt("wxWakeUpPipe::OnReadWaiting: read FAILED on fd=", GetReadFd());
        wxZombieDbgLogSigSafeInt("wxWakeUpPipe::OnReadWaiting: ... errno=", errno);

        wxLogSysError(_("Failed to read from wake-up pipe"));

        return;
    }

    // The pipe is empty now, so future calls to WakeUp() would need to write
    // to it again.
    //
    // Plain store, NOT exchange: if a signal handler on another thread
    // claimed the flag concurrently (exchange(true) in WakeUpNoLock) but
    // hasn't written its byte yet, we overwrite the claim with "empty" here.
    // That handler's byte will then arrive in an already-"empty" pipe,
    // producing at most one stale byte / one extra spurious wake-up later
    // (harmless - level-triggered dispatch handles it) - crucially it can
    // NEVER produce the inverse (flag stuck at "not empty" with an empty
    // pipe), which was the permanent-wedge zombie bug.
    m_pipeIsEmpty.store(true);

    // [zombie-debug] Confirm the pipe was drained and wake-ups re-enabled.
    if ( totalDrained > 0 )
        wxZombieDbgLogSigSafeInt("wxWakeUpPipe::OnReadWaiting: drained byte(s), read fd=", GetReadFd());
}
