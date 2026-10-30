// Scheduling priority for the two threads the picture and the sound hang on: on a
// laptop in a power-saving profile an ordinary thread's 2 ms sleep came back
// 5-75 ms late whenever VS Code, OBS or the compositor wanted the cores
// (2026-10-26). A desktop user has no RLIMIT_RTPRIO, so the requests go to rtkit
// (what PipeWire uses) over D-Bus; the direct call is tried first. Nothing here
// is fatal: refused, the thread stays ordinary and only the log says so.
//
// Two different asks, on purpose:
//  - the V-Sync tick thread gets SCHED_FIFO: its work is microseconds, and the
//    tick must land on time;
//  - the emulator thread gets a HIGH NICE priority, not realtime. A realtime
//    thread that runs RLIMIT_RTTIME (200 ms, rtkit's ceiling) without blocking is
//    KILLED by the kernel - the first build gave it FIFO before setup() and the
//    card scan (97 080 files) ended the process at "initFileSystem begin". A 28
//    MHz Next frame or a disk load can run that long too.
// Safety net for the FIFO thread: SIGXCPU at the soft limit demotes the thread to
// SCHED_OTHER instead of letting the hard limit kill it.
// PICOSPECCY_NO_RT=1 keeps everything ordinary (A/B test).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "pico_host.h"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#if HOST_RTKIT
#include <dbus/dbus.h>
#endif

#if HOST_RTKIT
// MakeThreadRealtime(tid, prio) or MakeThreadHighPriority(tid, nice).
static bool rtkit_call(const char* method, long tid, int arg) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection* bus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!bus) { dbus_error_free(&err); return false; }
    DBusMessage* m = dbus_message_new_method_call("org.freedesktop.RealtimeKit1",
                                                  "/org/freedesktop/RealtimeKit1",
                                                  "org.freedesktop.RealtimeKit1", method);
    if (!m) return false;
    const dbus_uint64_t t = (dbus_uint64_t)tid;
    if (strcmp(method, "MakeThreadRealtime") == 0) {
        const dbus_uint32_t p = (dbus_uint32_t)arg;
        dbus_message_append_args(m, DBUS_TYPE_UINT64, &t, DBUS_TYPE_UINT32, &p, DBUS_TYPE_INVALID);
    } else {
        const dbus_int32_t n = (dbus_int32_t)arg;
        dbus_message_append_args(m, DBUS_TYPE_UINT64, &t, DBUS_TYPE_INT32, &n, DBUS_TYPE_INVALID);
    }
    DBusMessage* r = dbus_connection_send_with_reply_and_block(bus, m, 2000, &err);
    dbus_message_unref(m);
    const bool ok = r != nullptr;
    if (r) dbus_message_unref(r);
    else dbus_error_free(&err);
    return ok;
}
#endif

// The kernel's "a realtime thread ran too long" warning: back to an ordinary
// thread, on the spot (sched_setscheduler is a plain syscall, signal-safe).
static void on_sigxcpu(int) {
    sched_param sp{};
    sched_setscheduler(0, SCHED_OTHER, &sp);
}

extern "C" void host_thread_realtime(const char* name, int prio) {
    if (getenv("PICOSPECCY_NO_RT")) return;
    // rtkit refuses a thread whose process has no runtime bound. Soft 100 ms
    // (SIGXCPU -> demote), hard 200 ms (rtkit's own ceiling).
    struct rlimit rl = { 100000, 200000 };
    setrlimit(RLIMIT_RTTIME, &rl);
    struct sigaction sa{};
    sa.sa_handler = on_sigxcpu;
    sigaction(SIGXCPU, &sa, nullptr);
    sched_param sp{};
    sp.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
        fprintf(stderr, "host: %s thread: SCHED_FIFO %d\n", name, prio);
        return;
    }
#if HOST_RTKIT
    if (rtkit_call("MakeThreadRealtime", (long)syscall(SYS_gettid), prio)) {
        fprintf(stderr, "host: %s thread: SCHED_FIFO %d via rtkit\n", name, prio);
        return;
    }
#endif
    fprintf(stderr, "host: %s thread stays an ordinary thread (no rtprio limit, no rtkit)\n", name);
}

extern "C" void host_thread_high_priority(const char* name, int nice_level) {
    if (getenv("PICOSPECCY_NO_RT")) return;
    const long tid = (long)syscall(SYS_gettid);
    if (setpriority(PRIO_PROCESS, (id_t)tid, nice_level) == 0) {
        fprintf(stderr, "host: %s thread: nice %d\n", name, nice_level);
        return;
    }
#if HOST_RTKIT
    if (rtkit_call("MakeThreadHighPriority", tid, nice_level)) {
        fprintf(stderr, "host: %s thread: nice %d via rtkit\n", name, nice_level);
        return;
    }
#endif
    fprintf(stderr, "host: %s thread stays at the ordinary priority (no rtkit)\n", name);
}
#else
extern "C" void host_thread_realtime(const char*, int) {}
extern "C" void host_thread_high_priority(const char*, int) {}
#endif
