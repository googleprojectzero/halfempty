/*
 * Copyright 2018 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef __linux__
# include <sys/prctl.h>
# include <sys/personality.h>
#endif
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include "proc.h"
#include "flags.h"
#include "util.h"

// This file is part of halfempty - a fast, parallel testcase minimization tool.
//
// Setup and execute child processes.
//

// This routine is called in the child process before the execve(), so it's
// useful for configuring limits and file descriptors, prctl and so on. Note
// that intefering with the heap will most likely deadlock the process (even
// calling g_message or whatever, which can cause a malloc()).
static void configure_child_limits(gpointer userdata)
{
    // Some of these may fail, not sure what to do.
    for (gint i = 0; i < RLIMIT_NLIMITS; i++) {
        if (setrlimit(i, &kChildLimits[i]) == -1) {
            g_critical("a call to setrlimit for %u failed(), %s", i, strerror(errno));
        }
    }

    // Make sure we create a new pgrp so that we can kill all subprocesses.
    setpgid(0, 0);

#ifdef __linux__
    // Try to cleanup if we get killed.
    prctl(PR_SET_PDEATHSIG, kKillFailedWorkersSignal);

    // Try to be as consistent as possible.
    personality(personality(~0) | ADDR_NO_RANDOMIZE);
#endif

    // Useful to help debug synchronization problems.
    if (kSleepSeconds)
        g_usleep(kSleepSeconds * G_USEC_PER_SEC);

    return;
}

// Splice data from a file descriptor into a pipe efficiently.
static gboolean write_pipe(gint pipefd,
                           gint datafd,
                           gsize size,
                           goffset dataoffset,
                           gboolean closepipe)
{
    ssize_t result;

    g_assert_cmpint(pipefd, >, 0);
    g_assert_cmpint(datafd, >, 0);

    while (size > 0) {
        result = splice(datafd, &dataoffset, pipefd, NULL, size, 0);

        if (result < 0) {
            break;
        } else {
            size -= result;
        }
    }

    if (closepipe) {
        g_close(pipefd, NULL);
    }

    // Probably broken pipe? I think this is okay, but should check that's the
    // real reason
    if (size != 0) {
        g_debug("failed to splice all data into pipe, %ld remaining", size);
    }

    return false;
}

// Handling timeouts in child processes.
//
// It's pretty normal for programs to take too long to process their input, so
// we need an option to support timeouts. I think --limit RLIMIT_CPU=10 works
// quite intuitively, but cannot be caught by users if they want to clean
// up and doesn't do anything if the process gets stuck blocking on something.
//
// We effectively want to run alarm() in the child, users can catch it (with
// trap) and cleanup, or just let it terminate everything if they don't care.
// But... alarm() won't work.  That only delivers a signal to the pgrp leader,
// which is not what anyone expects (they would expect all subprocesses to be
// cleaned up). I can't just write some code to catch that signal and forward
// it to the pgrp, because obviously that would be reset on execve. :-(
//
// I think there are only two options:
//
//      1. Just run alarm() in the child before execve, and require users to
//      handle timeouts in their shell scripts. It would need to be like this:
//
//      #!/bin/sh
//      trap 'kill $!' ALRM
//      childprogram & wait
//
//      Now the signal would work as expected, but not everyone is familiar
//      with this syntax, and it seems like a lot to ask for such a basic thing
//      as timeouts. I don't think many people understand pgrps, so it might be
//      kinda confusing.
//
//      2. I have a "timeout thread" that runs alarm() with a signal handler
//      that forwards the signal to the pgrp, not just the leader.
//
//      This would work as expected, but needs another thread and we have to
//      handle races killing us at unexpected times. Still, it would result in
//      the intuitive behaviour of a --timeout option.
//

typedef struct {
    GPid child;
    GCond condition;
} watchdog_t;

static gpointer timeout_watchdog_thread(gpointer param)
{
    siginfo_t info = {0};
    GMutex mutex;
    gint64 timeout;
    watchdog_t *data = param;

    g_mutex_init(&mutex);
    g_mutex_lock(&mutex);

    g_assert_nonnull(data);
    g_assert_cmpint(data->child, >, 0);

    g_debug("watchdog thread %p monitoring process %d",
            g_thread_self(),
            data->child);

    timeout = g_get_monotonic_time () + kMaxProcessTime * G_TIME_SPAN_SECOND;

    // Because g_cond_wait_until() can wakeup even if condition wasn't
    // signaled, check with waitid() to make sure it's not stopped.
    // If it has stopped, we need to 
    while (waitid(P_PID,
                  data->child,
                  &info,
                  WEXITED | WNOWAIT | WNOHANG) == 0) {

        // Check we are here because of WNOHANG, of if pid really exited.
        if (info.si_pid == data->child) {
            break;
        }

        // The only other possibility is WNOHANG.
        g_assert_cmpint(info.si_pid, ==, 0);

        // Wait for timeout, or main thread to tell us the child is dead.
        if (g_cond_wait_until(&data->condition, &mutex, timeout) == FALSE) {
            g_debug("condition timeout, watchdog will kill pgrp -%d",
                    data->child);

            // Timeout occurred, send a SIGALRM to the whole pgrp.
            if (kill(-data->child, SIGALRM)  != 0) {
                g_info("watchdog thread failed to kill child pgrp -%d",
                       data->child);
            }

            g_mutex_unlock(&mutex);
            g_mutex_clear(&mutex);
            return NULL;
        } else {
            g_debug("condition signaled, exit watchdog for pid %d",
                    data->child);
        }

        // The waitid() manual recommends zeroing si_pid to differentiate
        // WNOHANG from the case where no children exist.
        info.si_pid = 0;
    }

    g_mutex_unlock(&mutex);
    g_mutex_clear(&mutex);
    return NULL;
}

gint submit_data_subprocess(gint inputfd, gsize inputlen, GPid *childpid)
{
    GError  *error = NULL;
    GThread *watchdog = NULL;
    siginfo_t info = {0};
    watchdog_t timeout;
    gint pipein;
    gint result;
    gint flags;
    gchar **envp;
    gchar *argv[] = {
        kCommandPath,
        NULL,
    };

    // Make sure we're not being passed an old task.
    g_assert_nonnull(childpid);
    g_assert_cmpint(*childpid, ==, 0);

    // I want to reap the child myself.
    flags = G_SPAWN_DO_NOT_REAP_CHILD;

    if (kSilenceChildStdout)
        flags |= G_SPAWN_STDOUT_TO_DEV_NULL;

    if (kSilenceChildStderr)
        flags |= G_SPAWN_STDERR_TO_DEV_NULL;

    // Make any necessary changes to the childs environment.
    envp = g_get_environ();

    // glibc write mcheck() errors directly to /dev/tty, which spams the
    // console with error messages if a user is trying to minimize a heap
    // corruption bug.
    //
    // This disables that error message, unless the user has already configured
    // it to some other value.
    envp = g_environ_setenv(envp, "MALLOC_CHECK_", "2", false);

    // Create child process to verify data.
    if (g_spawn_async_with_pipes(NULL,
                                 argv,
                                 envp,
                                 flags,
                                 configure_child_limits,
                                 NULL,
                                 childpid,
                                 &pipein,
                                 NULL,
                                 NULL,
                                 &error) == FALSE) {
        g_error("failed to spawn child process, %s", error->message);
        g_assert_not_reached();
    }

    // Spawn the watchdog thread if necessary.
    if (kMaxProcessTime) {
        // Create a condition we signal.
        g_cond_init(&timeout.condition);

        // Pass the pid.
        timeout.child = *childpid;

        // Create thread.
        watchdog = g_thread_new("watchdog", timeout_watchdog_thread, &timeout);
    }

    g_debug("writing data to child %d pipefd=%d", *childpid, pipein);

    write_pipe(pipein, inputfd, inputlen, 0, true);

    g_debug("finished writing data to child, about to waitid(%d)", *childpid);

  childwait:
    // The data has been written to the child process, now we wait for it to
    // complete. We use NOWAIT so that the garbage collecting thread can reap
    // the children.
    if (waitid(P_PID, *childpid, &info, WEXITED | WNOWAIT) != 0) {
        // On macOS, waitid can fail with EINTR, I don't think this can happen
        // on Linux but it doesn't hurt to handle it.
        if (errno != EINTR) {
            g_error("waitid for child %d failed, %s",
                    *childpid,
                    strerror(errno));
        }

        // Continue waiting...
        goto childwait;
    }

    g_assert_cmpint(info.si_pid, ==, *childpid);

    // Terminate the watchdog thread, no longer necessary.
    if (kMaxProcessTime) {
        g_cond_signal(&timeout.condition);
        g_thread_join(watchdog);
    }

    switch (info.si_code) {
        case CLD_EXITED:
            g_debug("child %d exited with code %d",
                    info.si_pid,
                    info.si_status);

            // The exit code becomes our result.
            result = info.si_status;
            break;
        case CLD_DUMPED:
            g_debug("child %d dumped core, adjust limits?", *childpid);
            // fallthrough
        case CLD_KILLED:
            g_debug("child %d was killed by signal %s",
                    *childpid,
                    strsignal(info.si_status));
            result = -1;
            break;
        case CLD_STOPPED:
        case CLD_TRAPPED:
        default:
            g_assert_not_reached();
    }

    // Clean up child.
    g_clear_error(&error);
    g_strfreev(envp);
    return result;
}
