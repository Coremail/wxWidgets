///////////////////////////////////////////////////////////////////////////////
// Name:        src/gtk/evtloop.cpp
// Purpose:     implements wxEventLoop for GTK+
// Author:      Vadim Zeitlin
// Created:     10.07.01
// Copyright:   (c) 2001 Vadim Zeitlin <zeitlin@dptmaths.ens-cachan.fr>
//              (c) 2013 Rob Bresalier, Vadim Zeitlin
// Licence:     wxWindows licence
///////////////////////////////////////////////////////////////////////////////

// ============================================================================
// declarations
// ============================================================================

// ----------------------------------------------------------------------------
// headers
// ----------------------------------------------------------------------------

// For compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"


#include "wx/evtloop.h"
#include "wx/evtloopsrc.h"

#ifndef WX_PRECOMP
    #include "wx/app.h"
    #include "wx/log.h"
    #include "wx/event.h"
#endif // WX_PRECOMP

#include "wx/private/eventloopsourcesmanager.h"
#include "wx/apptrait.h"

#include "wx/gtk/private/wrapgtk.h"

// [zombie-debug] shared logging helper
#include "wx/unix/private/zombiedebug.h"

#include <dlfcn.h>
#include <link.h>

GdkWindow* wxGetTopLevelGDK();

// ============================================================================
// wxEventLoop implementation
// ============================================================================

// ----------------------------------------------------------------------------
// wxEventLoop running and exiting
// ----------------------------------------------------------------------------

namespace
{

// [zombie-debug] Heartbeat callback: runs from the DEFAULT GMainContext
// iteration itself.  If these lines stop appearing in the debug log while
// the app is still running, the default context is not being iterated
// anymore - which is exactly the zombie xdg-open signature we diagnosed
// (the signal wake-up pipe had a pending byte for 14h without ever being
// dispatched).
extern "C" gboolean wx_zombie_dbg_heartbeat(gpointer)
{
    wxZombieDbgLog("GlibHeartbeat: default context iterated, gtk_main_level=%d",
                   (int)gtk_main_level());
    return TRUE; // keep firing
}

} // anonymous namespace

wxGUIEventLoop::wxGUIEventLoop()
{
    m_exitcode = 0;

    // [zombie-debug] Install the heartbeat exactly once (nested event loops
    // create additional wxGUIEventLoop objects but we only need one source,
    // attached to the default context for the whole process lifetime).
    static bool s_heartbeatInstalled = false;
    if ( !s_heartbeatInstalled )
    {
        s_heartbeatInstalled = true;
        g_timeout_add_seconds(30, wx_zombie_dbg_heartbeat, nullptr);
        wxZombieDbgLog("GlibHeartbeat: heartbeat source installed (30s interval)");
    }
}

int wxGUIEventLoop::DoRun()
{
    guint loopLevel = gtk_main_level();

    // [zombie-debug] Log event loop entry: if we see this for the OUTER loop
    // and then never see its exit log, the loop was stuck inside gtk_main()
    // or inside a handler (e.g. the app's CEF pump).  If we see the exit log
    // of the outermost loop without a subsequent entry log, gtk_main was
    // never re-entered - events would be pumped by something else.
    wxZombieDbgLog("wxGUIEventLoop::DoRun: enter, gtk_main_level=%u", loopLevel);

    // This is placed inside of a loop to take into account nested
    // event loops.  For example, inside this event loop, we may receive
    // Exit() for a different event loop (which we are currently inside of)
    // That Exit() will cause this gtk_main() to exit so we need to re-enter it.
    while ( !m_shouldExit )
    {
        gtk_main();
    }

    // Force the enclosing event loop to also exit to see if it is done in case
    // that event loop had Exit() called inside of the just ended loop. If it
    // is not time yet for that event loop to exit, it will be executed again
    // due to the while() loop on m_shouldExit().
    //
    // This is unnecessary if we are the top level loop, i.e. loop of level 0.
    if ( loopLevel )
    {
        gtk_main_quit();
    }

    OnExit();

#if wxUSE_EXCEPTIONS
    // Rethrow any exceptions which could have been produced by the handlers
    // ran by the event loop.
    if ( wxTheApp )
        wxTheApp->RethrowStoredException();
#endif // wxUSE_EXCEPTIONS

    // [zombie-debug] Log event loop exit with the level AFTER gtk_main
    // returned - shows whether we were nested when we exited.
    wxZombieDbgLog("wxGUIEventLoop::DoRun: exit, gtk_main_level=%u",
                   gtk_main_level());

    return m_exitcode;
}

void wxGUIEventLoop::ScheduleExit(int rc)
{
    wxCHECK_RET( IsInsideRun(), wxT("can't call ScheduleExit() if not started") );

    m_exitcode = rc;

    m_shouldExit = true;

    gtk_main_quit();
}

void wxGUIEventLoop::WakeUp()
{
    // TODO: idle events handling should really be done by wxEventLoop itself
    //       but for now it's completely in gtk/app.cpp so just call there when
    //       we have wxTheApp and hope that it doesn't matter that we do
    //       nothing when we don't...
    if ( wxTheApp )
        wxTheApp->WakeUpIdle();
}

// ----------------------------------------------------------------------------
// wxEventLoop adding & removing sources
// ----------------------------------------------------------------------------

#if wxUSE_EVENTLOOP_SOURCE

extern "C"
{
static gboolean wx_on_channel_event(GIOChannel *channel,
                                    GIOCondition condition,
                                    gpointer data)
{
    wxUnusedVar(channel); // Unused if !wxUSE_LOG || !wxDEBUG_LEVEL

    // [zombie-debug] Log EVERY glib dispatch for ANY watched fd, including
    // the condition bits.  This is the GTK-level counterpart of
    // SignalsWakeUpPipe::OnReadWaiting: if HandleSignal/WakeUpNoLock logged
    // but this never fires for the signal pipe fd, the glib watch itself is
    // gone or the fd is dead.
    //
    // G_IO_HUP / G_IO_ERR / G_IO_NVAL here for the signal pipe fd means the
    // read end was closed (or never valid) - that's the smoking gun for the
    // "287 SIGCHLDs never dispatched" pattern from the previous logs.
    {
        int watchedFd = g_io_channel_unix_get_fd(channel);
        FILE* dbg = fopen("/tmp/wx_execute_log", "a");
        if ( dbg )
        {
            fprintf(dbg, "wx_on_channel_event: fd=%d condition=%08x%s%s%s%s%s\n",
                    watchedFd, (unsigned)condition,
                    (condition & G_IO_IN)   ? " G_IO_IN"   : "",
                    (condition & G_IO_PRI)  ? " G_IO_PRI"  : "",
                    (condition & G_IO_HUP)  ? " G_IO_HUP"  : "",
                    (condition & G_IO_ERR)  ? " G_IO_ERR"  : "",
                    (condition & G_IO_NVAL) ? " G_IO_NVAL" : "");
            fclose(dbg);
        }
    }

    wxLogTrace(wxTRACE_EVT_SOURCE,
               "wx_on_channel_event, fd=%d, condition=%08x",
               g_io_channel_unix_get_fd(channel), condition);

    wxEventLoopSourceHandler * const
        handler = static_cast<wxEventLoopSourceHandler *>(data);

    if ( (condition & G_IO_IN) || (condition & G_IO_PRI) || (condition & G_IO_HUP) )
        handler->OnReadWaiting();

    if (condition & G_IO_OUT)
        handler->OnWriteWaiting();

    if ( (condition & G_IO_ERR) || (condition & G_IO_NVAL) )
        handler->OnExceptionWaiting();

    // we never want to remove source here, so always return true
    //
    // The source may have been removed by the handler, so it may be
    // a good idea to return FALSE when the source has already been
    // removed.  However, that would involve somehow informing this function
    // that the source was removed, which is not trivial to implement
    // and handle all cases.  It has been found through testing
    // that if the source was removed by the handler, that even if we
    // return TRUE here, the source/callback will not get called again.
    return TRUE;
}
}

class wxGUIEventLoopSourcesManager : public wxEventLoopSourcesManagerBase
{
public:
    virtual wxEventLoopSource*
    AddSourceForFD(int fd, wxEventLoopSourceHandler *handler, int flags) wxOVERRIDE
    {
        wxCHECK_MSG( fd != -1, NULL, "can't monitor invalid fd" );

        int condition = 0;
        if ( flags & wxEVENT_SOURCE_INPUT )
            condition |= G_IO_IN | G_IO_PRI | G_IO_HUP;
        if ( flags & wxEVENT_SOURCE_OUTPUT )
            condition |= G_IO_OUT;
        if ( flags & wxEVENT_SOURCE_EXCEPTION )
            condition |= G_IO_ERR | G_IO_NVAL;

        GIOChannel* channel = g_io_channel_unix_new(fd);
        const unsigned sourceId  = g_io_add_watch
                                   (
                                    channel,
                                    (GIOCondition)condition,
                                    &wx_on_channel_event,
                                    handler
                                   );
        // it was ref'd by g_io_add_watch() so we can unref it here
        g_io_channel_unref(channel);

        // [zombie-debug] Log every glib watch registration with its id, so
        // we can correlate later watch removals and callback dispatches.
        {
            FILE* dbg = fopen("/tmp/wx_execute_log", "a");
            if ( dbg )
            {
                fprintf(dbg,
                        "AddSourceForFD: glib watch added, fd=%d sourceId=%u condition=%08x%s\n",
                        fd, sourceId, condition,
                        sourceId ? "" : " (FAILED!)");
                fclose(dbg);
            }
        }

        if ( !sourceId )
            return NULL;

        wxLogTrace(wxTRACE_EVT_SOURCE,
                   "Adding event loop source for fd=%d with GTK id=%u",
                   fd, sourceId);


        return new wxGTKEventLoopSource(sourceId, handler, flags);
    }
};

wxEventLoopSourcesManagerBase* wxGUIAppTraits::GetEventLoopSourcesManager()
{
    static wxGUIEventLoopSourcesManager s_eventLoopSourcesManager;

    return &s_eventLoopSourcesManager;
}

wxGTKEventLoopSource::~wxGTKEventLoopSource()
{
    // [zombie-debug] Log every glib watch removal with its id.  If the watch
    // for the signal pipe fd (see SignalsWakeUpPipe creation log) is removed
    // while the app is still running, all subsequent SIGCHLD wake-ups are
    // silently lost -> tracked children become zombies.  This catches the
    // "source deleted/detached from context" hypothesis.
    {
        FILE* dbg = fopen("/tmp/wx_execute_log", "a");
        if ( dbg )
        {
            fprintf(dbg, "wxGTKEventLoopSource DTOR: g_source_remove sourceId=%u\n",
                    m_sourceId);
            fclose(dbg);
        }
    }

    wxLogTrace(wxTRACE_EVT_SOURCE,
               "Removing event loop source with GTK id=%u", m_sourceId);

    g_source_remove(m_sourceId);
}

#endif // wxUSE_EVENTLOOP_SOURCE

// ----------------------------------------------------------------------------
// wxEventLoop message processing dispatching
// ----------------------------------------------------------------------------

bool wxGUIEventLoop::Pending() const
{
    if ( wxTheApp )
    {
        // this avoids false positives from our idle source
        return wxTheApp->EventsPending();
    }

    return gtk_events_pending() != 0;
}

bool wxGUIEventLoop::Dispatch()
{
    wxCHECK_MSG( IsRunning(), false, wxT("can't call Dispatch() if not running") );

    // gtk_main_iteration() returns TRUE only if gtk_main_quit() was called
    return !gtk_main_iteration();
}

extern "C" {
static gboolean wx_event_loop_timeout(void* data)
{
    bool* expired = static_cast<bool*>(data);
    *expired = true;

    // return FALSE to remove this timeout
    return FALSE;
}
}

int wxGUIEventLoop::DispatchTimeout(unsigned long timeout)
{
    bool expired = false;
    const unsigned id = g_timeout_add(timeout, wx_event_loop_timeout, &expired);
    bool quit = gtk_main_iteration() != 0;

    if ( expired )
        return -1;

    g_source_remove(id);

    return !quit;
}

//-----------------------------------------------------------------------------
// YieldFor
//-----------------------------------------------------------------------------

extern "C" {
static void wxgtk_main_do_event(GdkEvent* event, void* data)
{
    // categorize the GDK event according to wxEventCategory.
    // See http://library.gnome.org/devel/gdk/unstable/gdk-Events.html#GdkEventType
    // for more info.

    // NOTE: GDK_* constants which were not present in the GDK2.0 can be tested for
    //       only at compile-time; when running the program (compiled with a recent GDK)
    //       on a system with an older GDK lib we can be sure there won't be problems
    //       because event->type will never assume those values corresponding to
    //       new event types (since new event types are always added in GDK with non
    //       conflicting values for ABI compatibility).

    // Some events (currently only a single one) may be used for more than one
    // category, so we need 2 variables. The second one will remain "unknown"
    // in most cases.
    wxEventCategory cat = wxEVT_CATEGORY_UNKNOWN,
                    cat2 = wxEVT_CATEGORY_UNKNOWN;
    switch (event->type)
    {
    case GDK_SELECTION_REQUEST:
    case GDK_SELECTION_NOTIFY:
    case GDK_SELECTION_CLEAR:
    case GDK_OWNER_CHANGE:
        cat = wxEVT_CATEGORY_CLIPBOARD;
        break;

    case GDK_KEY_PRESS:
    case GDK_KEY_RELEASE:
    case GDK_BUTTON_PRESS:
    case GDK_2BUTTON_PRESS:
    case GDK_3BUTTON_PRESS:
    case GDK_BUTTON_RELEASE:
    case GDK_SCROLL:        // generated from mouse buttons
    case GDK_CLIENT_EVENT:
        cat = wxEVT_CATEGORY_USER_INPUT;
        break;

    case GDK_PROPERTY_NOTIFY:
        // This one is special: it can be used for UI purposes but also for
        // clipboard operations, so allow it in both cases (we probably could
        // examine the event itself to distinguish between the two cases but
        // this would be unnecessarily complicated).
        cat2 = wxEVT_CATEGORY_CLIPBOARD;
        wxFALLTHROUGH;

    case GDK_PROXIMITY_IN:
    case GDK_PROXIMITY_OUT:

    case GDK_MOTION_NOTIFY:
    case GDK_ENTER_NOTIFY:
    case GDK_LEAVE_NOTIFY:
    case GDK_VISIBILITY_NOTIFY:

    case GDK_FOCUS_CHANGE:
    case GDK_CONFIGURE:
    case GDK_WINDOW_STATE:
    case GDK_SETTING:
    case GDK_DELETE:
    case GDK_DESTROY:

    case GDK_EXPOSE:
#ifndef __WXGTK3__
    case GDK_NO_EXPOSE:
#endif
    case GDK_MAP:
    case GDK_UNMAP:

    case GDK_DRAG_ENTER:
    case GDK_DRAG_LEAVE:
    case GDK_DRAG_MOTION:
    case GDK_DRAG_STATUS:
    case GDK_DROP_START:
    case GDK_DROP_FINISHED:
#if GTK_CHECK_VERSION(2,8,0)
    case GDK_GRAB_BROKEN:
#endif
#if GTK_CHECK_VERSION(2,14,0)
    case GDK_DAMAGE:
#endif
        cat = wxEVT_CATEGORY_UI;
        break;

    default:
        cat = wxEVT_CATEGORY_UNKNOWN;
        break;
    }

    wxGUIEventLoop* evtloop = static_cast<wxGUIEventLoop*>(data);

    // is this event allowed now?
    if (evtloop->IsEventAllowedInsideYield(cat) ||
            (cat2 != wxEVT_CATEGORY_UNKNOWN &&
                evtloop->IsEventAllowedInsideYield(cat2)))
    {
        // process it now
        gtk_main_do_event(event);
    }
    else if (event->type != GDK_NOTHING)
    {
        // process it later (but make a copy; the caller will free the event
        // pointer)
        evtloop->StoreGdkEventForLaterProcessing(gdk_event_copy(event));
    }
}
}

bool isUsingCef(){
    static bool is_using_cef = false;
    static bool known_already = false;
    if (known_already)
        return is_using_cef;
     // Use dl_iterate_phdr to iterate over the program headers of the current process
    dl_iterate_phdr([](struct dl_phdr_info *info, size_t WXUNUSED(size), void *WXUNUSED(data)) {
        auto so_name = std::string{info->dlpi_name};
        auto pos = so_name.find_last_of('/');
        if (pos == std::string::npos)
            return 0;
        so_name = so_name.substr(pos + 1);
        if (so_name.compare(0, 9, "libcef.so") == 0){
            is_using_cef = true;
            return 1;
        }
        return 0;
    }, nullptr);
    known_already = true;
    return is_using_cef;
}

void wxGUIEventLoop::DoYieldFor(long eventsToProcess)
{	
    // DO NOT replace the global GDK event handler with our 'wxgtk_main_do_event' WHEN-USING-CEF.
    // Because this trick rely on one uncertain assumption:
    // No one besides us, had done gdk_event_handler_set() already.
    // In most case, this might be true.
    // But a single exception can destroy this trick completely.
    // For example, when we want to embed CEF, Chromium will use a custom gdk_event_handler too.
    // This trick will render all CEF browser window/view no longer usable.
    // GTK3 should be blamed for not offering any gdk_event_handler_get() or similar stuff for a safe trick here.
    // In GTK4, there's a more sane way to get the job done.
    // So let's do it in a more conservative way.
    // If there're GdkEvents, we handle them via 'wxgtk_main_do_event',
    // all other events should be handle by one gtk_main_iteration().
    // I'm not sure whether this is really okay, but it seems a nicer and less intrusive way to do things.
    if (isUsingCef()){
        while(Pending()){
            auto gdk_event_ = gdk_event_get();
            if (gdk_event_ != nullptr){
                wxgtk_main_do_event(gdk_event_, this);
                gdk_event_free(gdk_event_);
            }
            else {
                // while no local gdk events pending,
                // we should schedule one time trigger to allow events for CEF
                // to be handled, this is essential to unblock us from clipboard
                // interaction with CEF.
                // this bogus event will let the DoMessageLoopWork() kick in.
                wxCommandEvent cef_jobs_trigger{};
                if (wxTheApp)
                    wxTheApp->ProcessEvent(dynamic_cast<wxEvent&>(cef_jobs_trigger));
                break;
            }
        }
	}
	else {
        // temporarily replace the global GDK event handler with our function, which
        // categorizes the events and using m_eventsToProcessInsideYield decides
        // if an event should be processed immediately or not
        // NOTE: this approach is better than using gdk_display_get_event() because
        //       gtk_main_iteration() does more than just calling gdk_display_get_event()
        //       and then call gtk_main_do_event()!
        //       In particular in this way we also process input from sources like
        //       GIOChannels (this is needed for e.g. wxGUIAppTraits::WaitForChild).
        gdk_event_handler_set(wxgtk_main_do_event, this, NULL);
        while (Pending())   // avoid false positives from our idle source
            gtk_main_iteration();

        wxGCC_WARNING_SUPPRESS_CAST_FUNCTION_TYPE()
        gdk_event_handler_set ((GdkEventFunc)gtk_main_do_event, NULL, NULL);
        wxGCC_WARNING_RESTORE_CAST_FUNCTION_TYPE()
	}

    wxEventLoopBase::DoYieldFor(eventsToProcess);

    // put any unprocessed GDK events back in the queue
    if ( !m_arrGdkEvents.IsEmpty() )
    {
        GdkDisplay* disp = gdk_window_get_display(wxGetTopLevelGDK());
        for (size_t i=0; i<m_arrGdkEvents.GetCount(); i++)
        {
            GdkEvent* ev = (GdkEvent*)m_arrGdkEvents[i];

            // NOTE: gdk_display_put_event makes a copy of the event passed to it
            gdk_display_put_event(disp, ev);
            gdk_event_free(ev);
        }

        m_arrGdkEvents.Clear();
    }
}
