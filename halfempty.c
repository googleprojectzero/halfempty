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
#ifdef __APPLE__
# include <sys/sysctl.h>
#endif
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include "task.h"
#include "proc.h"
#include "util.h"
#include "tree.h"
#include "limits.h"
#include "flags.h"

// This file is part of halfempty - a fast, parallel testcase minimization tool.
//
// Commandline options parsing and main() function.
//

#define HALFEMPTY_VERSION_STRING "0.20"

void signal_handler(int sig, siginfo_t *info, void *ucontext)
{
#if __linux__
    g_debug("%s received from %d", strsignal(sig), info->si_pid);
#endif
}

static const GOptionEntry kThreadOptions[] = {
    { "num-threads", 'P', G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &kProcessThreads, "How many threads to use (default=ncores+1).", "threads" },
    { "cleanup-threads", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &kCleanupThreads, "Number of threads used to garbage collect (default=4).", "threads" },
    { "max-queue", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &kMaxUnprocessed, "Maximum number of unprocessed workunits (default=4).", "N" },
    { "poll-delay", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &kWorkerPollDelay, "How long to sleep between checking queue status (default=10000).", "usecs" },
    { NULL },
};

static const GOptionEntry kDebugOptions[] = {
    { "generate-dot", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &kGenerateDotFile, "Generate a DOT file to display the tree status (default=off).", NULL },
    { "collapse", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &kSimplifyDotFile, "Collapse branches in generated dot files (default=off).", NULL },
    { "verbosity", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &kVerbosity, "Set verbosity level (default=0).", "level" },
    { "sleep", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_INT, &kSleepSeconds, "Sleep this many seconds before execve() (default=0).", "seconds" },
    { NULL },
};

static const GOptionEntry kProcessOptions[] = {
    { "no-terminate", 'k', G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &kKillFailedWorkers, "Don't terminate tests early if possible (default=terminate).", NULL },
    { "term-signal", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &kKillFailedWorkersSignal, "Signal to send discarded workers (default=15).", "signal" },
    { "timeout", 'T', G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &kMaxProcessTime, "Maximum child execution time (default=unlimited).", "seconds" },
    { "limit", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK, decode_proc_limit, "Configure a child limit (ex. RLIMIT_CPU=60).", "RLIMIT_RESOURCE=N" },
    { "inherit-stdout", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &kSilenceChildStdout, "Don't redirect child stdout to /dev/null (default=redirect)", NULL },
    { "inherit-stderr", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &kSilenceChildStderr, "Don't redirect child stderr to /dev/null (default=redirect)", NULL },
    { NULL },
};

static const GOptionEntry kStandardOptions[] = {
    { "output", 'o', G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &kOutputFile, "Location to store minimized output (default=halfempty.out).", "filename" },
    { "stable", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &kIterateUntilStable, "Re-run strategies until the result is stable (default=false).", NULL },
    { "quiet", 'q', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &kQuiet, "Minimize all informational messages, try to only print errors (default=false).", NULL },
    { "continue", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &kContinueSearch, "Don't exit when finished, keep trying until interrupted (default=false).", NULL },
    { "noverify", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &kVerifyInput, "Don't verify original input (faster, but can cause confusing errors) (default=false).", NULL },
    { "monitor", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &kMonitorMode, "Run dot (if installed) to monitor progress in your web browser (default=false).", NULL },
    { NULL },
};

static void show_halfempty_banner()
{
    static const gchar *kReset = "\e[0m";
    static const gchar *kGlass = "\e[36m";
    static const gchar *kMilk  = "\e[47m\e[30m";

    // It's hard to make something look like a glass in ascii :-)
    g_print("%s╭│   │%s ── halfempty ───────────────────────────────────────────────── v%s ──\n"
            "%s╰│%s%3d%s%s│%s A fast, parallel testcase minimization tool\n"
            "%s ╰───╯%s ───────────────────────────────────────────────────────── by @taviso ──\n",
           kGlass, kReset, HALFEMPTY_VERSION_STRING,
           kGlass, kMilk, g_get_num_processors(), kReset, kGlass, kReset,
           kGlass, kReset);
}

int main(int argc, char **argv)
{
    gint output;
    gint fd;
    GOptionContext *context;
    GOptionGroup *threadopts;
    GOptionGroup *debugopts;
    GOptionGroup *procopts;

    struct sigaction sa =  {
        .sa_sigaction = signal_handler,
        .sa_flags = SA_SIGINFO,
    };

    // Setup some default options.
    kProcessThreads = g_get_num_processors() + 1;

    // Setup a print handler that respects the kQuiet parameter.
    g_set_print_handler(g_print_quiet);

    threadopts  = g_option_group_new("threads",
                                     "Fine tune performance options:",
                                     "Performance options",
                                     NULL,
                                     NULL);
    debugopts   = g_option_group_new("debug",
                                     "Debug halfempty problems:",
                                     "Debugging options",
                                     NULL,
                                     NULL);
    procopts    = g_option_group_new("process",
                                     "Control test program execution:",
                                     "Test program options",
                                     NULL,
                                     NULL);

    g_option_group_add_entries(threadopts, kThreadOptions);
    g_option_group_add_entries(debugopts, kDebugOptions);
    g_option_group_add_entries(procopts, kProcessOptions);

    // Hide very noisy debug messages
    //g_log_set_handler(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, g_log_null_handler, NULL);
    //g_log_set_handler(G_LOG_DOMAIN, G_LOG_LEVEL_INFO, g_log_default_handler, NULL);

    context = g_option_context_new("SCRIPT INPUTFILE");

    g_option_context_add_main_entries (context, kStandardOptions, NULL);
    g_option_context_add_group(context, threadopts);
    g_option_context_add_group(context, debugopts);
    g_option_context_add_group(context, procopts);

    // Initialize strategy specific options.
    g_info("Initializing %u strategies...", kNumStrategies);
    for (gint k = 0; k < kNumStrategies; k++) {
        GOptionGroup *stratopts = g_option_group_new(kStrategyList[k].name,
                                                     "Fine tune specific strategy options:",
                                                     "Strategy Specific Options",
                                                     NULL,
                                                     NULL);
        g_option_group_add_entries(stratopts, kStrategyList[k].options);
        g_option_context_add_group(context, stratopts);
        g_info("Strategy %u: \"%s\", %s", k + 1, kStrategyList[k].name, kStrategyList[k].description);
    }

    // Parse all options.
    if (g_option_context_parse(context, &argc, &argv, NULL) == false) {
        g_warning("parsing commandline options failed, see --help for information");
        return EXIT_FAILURE;
    }

    show_halfempty_banner();

    // We send the input data to the test program via pipes, so if the child
    // crashes or exits before we send it all the data we have, we get SIGPIPE.
    sigaction(SIGPIPE, &sa, NULL);

    {
        struct rlimit limfiles = {
            .rlim_cur = 32768,
            .rlim_max = RLIM_INFINITY,
        };

#ifdef __APPLE__
        size_t length = sizeof(rlim_t);

        // The default for RLIMIT_NOFILE on macOS is insanely small (256), lets
        // turn it up to something reasonable.
        if (sysctlbyname("kern.maxfilesperproc", &limfiles.rlim_cur, &length, NULL, 0) != 0) {
            g_warning("failed to query kern.maxfilesperproc");
        }
#else
        // Try to turn up the rlimit for RLIMIT_NOFILE, at least on some
        // distributions the default soft limit is too small for halfempty (e.g.
        // RHEL7).
        if (getrlimit(RLIMIT_NOFILE, &limfiles) != 0) {
            g_warning("failed to query limits, use \"ulimit -n\" instead");
        } else {
            // Use the maximum we're allowed.
            limfiles.rlim_cur = limfiles.rlim_max;
        }
#endif

        if (setrlimit(RLIMIT_NOFILE, &limfiles) != 0) {
            g_warning("failed to adjust resource limits, use \"ulimit -n\" if necessary");
        }
    }


    if (argc != 3) {
        g_warning("You must specify two parameters, a test program and an inputfile");
        return EXIT_FAILURE;
    }

    // First parameter is the script.
    if (g_access(kCommandPath = argv[1], X_OK) != 0) {
        g_warning("The test program `%s` does not seem to be executable.", kCommandPath);
        return EXIT_FAILURE;
    }

    // The remaining parameter should be the input file.
    if (g_access(kInputFile = argv[2], R_OK) != 0) {
        g_warning("The inputfile specified `%s` does not seem valid.", kInputFile);
        return EXIT_FAILURE;
    }

    // Prepare the root node with the initial input data.
    if ((fd = g_open(kInputFile, O_RDONLY)) < 0) {
        g_warning("failed to open the specified input file, %s", kInputFile);
        return EXIT_FAILURE;
    }

    // Begin minimization.
    while (true) {
        // Record original size.
        gsize originalsize = g_file_size(fd);

        // Iterate over all available strategies.
        for (gint k = 0; k < kNumStrategies; k++) {
            g_print("Input file \"%s\" is now %lu bytes, starting strategy \"%s\"...", kInputFile, g_file_size(fd), kStrategyList[k].name);

            if (build_bisection_tree(fd, kStrategyList[k].callback, &fd, BISECT_FLAG_CLOSEINPUT) == false) {
                g_print("Strategy \"%s\" failed, cannot continue.", kStrategyList[k].name);
                return EXIT_FAILURE;
            }

            g_print("");

            g_print("Strategy \"%s\" complete, output %lu bytes", kStrategyList[k].name, g_file_size(fd));
        }

        if (kIterateUntilStable && g_file_size(fd) < originalsize) {
            g_print("Minimization succeeded, testing if minimization is stable...\n");
            continue;
        } else if (kIterateUntilStable) {
            g_print("Minimization stable, all work done.\n");
        }

        // All done.
        break;
    }

    g_print("All work complete, generating output %s (size: %lu)", kOutputFile, g_file_size(fd));

    output = g_open(kOutputFile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    g_sendfile_all(output, fd, 0, g_file_size(fd));
    g_close(output, NULL);
    g_close(fd, NULL);
    g_option_context_free(context);
    return EXIT_SUCCESS;
}
