/////////////////////////////////////////////////////////////////////////////
// Name:        src/unix/appunix.cpp
// Purpose:     wxAppConsole with wxMainLoop implementation
// Author:      Lukasz Michalski
// Created:     28/01/2005
// Copyright:   (c) Lukasz Michalski
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////

#include "wx/wxprec.h"


#ifndef WX_PRECOMP
    #include "wx/app.h"
    #include "wx/log.h"
#endif

#include "wx/evtloop.h"
#include "wx/scopedptr.h"
#include "wx/unix/private/wakeuppipe.h"
#include "wx/private/fdiodispatcher.h"
#include "wx/private/fdioeventloopsourcehandler.h"

#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

// [zombie-debug] shared logging helpers (async-signal-safe + wxLog-based)
#include "wx/unix/private/zombiedebug.h"

#ifndef SA_RESTART
    // don't use for systems which don't define it (at least VMS and QNX)
    #define SA_RESTART 0
#endif

// ----------------------------------------------------------------------------
// Helper class calling CheckSignal() on wake up
// ----------------------------------------------------------------------------

namespace
{

class SignalsWakeUpPipe : public wxWakeUpPipe
{
public:
    // Ctor automatically registers this pipe with the event loop.
    SignalsWakeUpPipe()
    {
        // [zombie-debug] Log the pipe creation and its fd.  If the fd is
        // later closed externally, this is our baseline to detect it.
        wxZombieDbgLog("SignalsWakeUpPipe: created, read fd=%d",
                       GetReadFd());

        m_source = wxEventLoopBase::AddSourceForFD
                                    (
                                        GetReadFd(),
                                        this,
                                        wxEVENT_SOURCE_INPUT
                                    );

        // [zombie-debug] Log whether source registration succeeded.  If this
        // fails, SIGCHLD wake-ups are NEVER dispatched -> guaranteed zombies.
        if ( !m_source )
            wxZombieDbgLog("SignalsWakeUpPipe: ERROR AddSourceForFD FAILED for fd=%d!",
                           GetReadFd());
        else
            wxZombieDbgLog("SignalsWakeUpPipe: source registered OK for fd=%d",
                           GetReadFd());
    }

    virtual void OnReadWaiting() wxOVERRIDE
    {
        // [zombie-debug] This is the CRITICAL dispatch point that was silent
        // in the previous logs: if SIGCHLD was received (HandleSignal logged)
        // but this function never logs, the event loop is not dispatching
        // the wake-up pipe watch (source removed / fd closed / glib watch
        // broken).  Log fd validity too: if fd became invalid (e.g. closed
        // and reused by other code), that's the root cause.
        int fd = GetReadFd();
        int ncmd = fcntl(fd, F_GETFD);
        wxZombieDbgLog("SignalsWakeUpPipe::OnReadWaiting: entered, read fd=%d (fcntl F_GETFD=%d)",
                       fd, ncmd);

        // The base class wxWakeUpPipe::OnReadWaiting() needs to be called in order
        // to read the data out of the wake up pipe and clear it for next time.
        wxWakeUpPipe::OnReadWaiting();

        if ( wxTheApp )
            wxTheApp->CheckSignal();
        else
            wxZombieDbgLog("SignalsWakeUpPipe::OnReadWaiting: wxTheApp is NULL, CheckSignal skipped");
    }

    virtual ~SignalsWakeUpPipe()
    {
        // [zombie-debug] Log destruction: if this fires before the children
        // were reaped, the SIGCHLD watch is gone -> remaining tracked children
        // become zombies.
        wxZombieDbgLog("SignalsWakeUpPipe: DESTROYED (deleting source), read fd=%d",
                       GetReadFd());
        delete m_source;
    }

private:
    wxEventLoopSource* m_source;
};

} // anonymous namespace

wxAppConsole::wxAppConsole()
{
    m_signalWakeUpPipe = NULL;
}

wxAppConsole::~wxAppConsole()
{
    delete m_signalWakeUpPipe;
}

// use unusual names for arg[cv] to avoid clashes with wxApp members with the
// same names
bool wxAppConsole::Initialize(int& argc_, wxChar** argv_)
{
    if ( !wxAppConsoleBase::Initialize(argc_, argv_) )
        return false;

    sigemptyset(&m_signalsCaught);

    return true;
}

// The actual signal handler. It does as little as possible (because very few
// things are safe to do from inside a signal handler) and just ensures that
// CheckSignal() will be called later from SignalsWakeUpPipe::OnReadWaiting().
void wxAppConsole::HandleSignal(int signal)
{
    // [zombie-debug] Async-signal-safe logging — this is the FIRST link in the
    // child-reaping chain. If SIGCHLD is never received here, the child will
    // become a zombie because waitpid() is never called downstream.
    // We must NOT use wxLog/fstream here (not async-signal-safe).
    if ( signal == SIGCHLD )
        wxZombieDbgLogSigSafe("HandleSignal: SIGCHLD received");

    wxAppConsole * const app = wxTheApp;
    if ( !app )
    {
        // [zombie-debug] wxTheApp is null — SIGCHLD received but no app to
        // process it, child WILL become a zombie.
        if ( signal == SIGCHLD )
            wxZombieDbgLogSigSafe("HandleSignal: SIGCHLD but wxTheApp is NULL!");
        return;
    }

    // Register the signal that is caught.
    sigaddset(&(app->m_signalsCaught), signal);

    // Wake up the application for handling the signal.
    //
    // Notice that we must have a valid wake up pipe here as we only install
    // our signal handlers after allocating it.
    app->m_signalWakeUpPipe->WakeUpNoLock();
}

void wxAppConsole::CheckSignal()
{
    // [zombie-debug] Log both branches here:
    //  - SIGCHLD pending -> normal dispatch (chain works up to here)
    //  - called but NOTHING pending -> OnReadWaiting fired for a reason other
    //    than SIGCHLD (spurious wake-up, or a signal got lost between the
    //    handler setting the flag and this call - e.g. handler ran on a
    //    different app object).  Distinguishing these narrows the root cause.
    bool sigchldPending = sigismember(&m_signalsCaught, SIGCHLD) != 0;
    bool hasHandler = m_signalHandlerHash.find(SIGCHLD) != m_signalHandlerHash.end();
    if ( sigchldPending )
    {
        wxZombieDbgLog("CheckSignal: SIGCHLD is pending, dispatching handlers");
    }
    else if ( hasHandler )
    {
        wxZombieDbgLog("CheckSignal: called but SIGCHLD NOT pending (spurious wake-up or flag lost?), handler registered=%d",
                       hasHandler ? 1 : 0);
    }

    for ( SignalHandlerHash::iterator it = m_signalHandlerHash.begin();
          it != m_signalHandlerHash.end();
          ++it )
    {
        int sig = it->first;
        if ( sigismember(&m_signalsCaught, sig) )
        {
            sigdelset(&m_signalsCaught, sig);
            (it->second)(sig);
        }
    }
}

wxFDIOHandler* wxAppConsole::RegisterSignalWakeUpPipe(wxFDIODispatcher& dispatcher)
{
    wxCHECK_MSG( m_signalWakeUpPipe, NULL, "Should be allocated" );

    // we need a bridge to wxFDIODispatcher
    //
    // TODO: refactor the code so that only wxEventLoopSourceHandler is used
    wxScopedPtr<wxFDIOHandler>
        fdioHandler(new wxFDIOEventLoopSourceHandler(m_signalWakeUpPipe));

    if ( !dispatcher.RegisterFD
                     (
                      m_signalWakeUpPipe->GetReadFd(),
                      fdioHandler.get(),
                      wxFDIO_INPUT
                     ) )
        return NULL;

    return fdioHandler.release();
}

// the type of the signal handlers we use is "void(*)(int)" while the real
// signal handlers are extern "C" and so have incompatible type and at least
// Sun CC warns about it, so use explicit casts to suppress these warnings as
// they should be harmless
extern "C"
{
    typedef void (*SignalHandler_t)(int);
}

bool wxAppConsole::SetSignalHandler(int signal, SignalHandler handler)
{
    const bool install = (SignalHandler_t)handler != SIG_DFL &&
                         (SignalHandler_t)handler != SIG_IGN;

    if ( !m_signalWakeUpPipe )
    {
        // Create the pipe that the signal handler will use to cause the event
        // loop to call wxAppConsole::CheckSignal().
        m_signalWakeUpPipe = new SignalsWakeUpPipe();
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = (SignalHandler_t)&wxAppConsole::HandleSignal;
    sa.sa_flags = SA_RESTART;
	errno = 0;
    int res = sigaction(signal, &sa, 0);
    if ( res != 0 )
    {
		(void) std::ofstream("/tmp/wx_execute_log", std::ios::app);
		std::fstream xdg_log("/tmp/wx_execute_log", std::ios::in | std::ios::out | std::ios::app);
		auto logger_ = wxLogStream(&xdg_log);
		wxLog::SetActiveTarget(&logger_);
        //wxLogSysError(_("Failed to install signal handler"));
		wxLogInfo("wxAppConsole::SetSignalHandler: Failed to install %d signal handler. errno: %d\n", signal, errno);
		wxLog::SetActiveTarget(nullptr);
        return false;
    }

    if ( install )
        m_signalHandlerHash[signal] = handler;
    else
        m_signalHandlerHash.erase(signal);

    return true;
}

