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
#include <stdbool.h>
#include <sys/resource.h>

#include "flags.h"

// This file is part of halfempty - a fast, parallel testcase minimization
// tool.
//
// This file contains global knobs that can be controlled via command line
// options.
//

// Maximum number of unprocessed workunits before we stop generating more.
// Each of these consumes a file descriptor, so cannot be infinite. Large
// numbers might speed up minimizing very slow files, otherwise keep it small.
// The problem is if you set this too high, we might go down the wrong path too
// far and pay a performance penalty to recover.
guint kMaxUnprocessed = 2;

// Number of threads dedicated to executing tests.
// Unless overridden at runtime, this is set to number of available cores.
guint kProcessThreads = 32;

// Number of threads dedicated to cleaning up resources (~4 is reasonable).
// These threads mostly wait on locks and hardly consume any resources.
guint kCleanupThreads = 4;

// How long to sleep between checking if we need more work.
guint kWorkerPollDelay = 10000;

// Maximum amount of time we will wait to see if we need to create more work.
guint kMaxWaitTime = 5 * G_TIME_SPAN_SECOND;

// If the tree gets too big, we start spending a lot of time traversing it. We
// can collapse long paths of consecutive failures into one, compressing the
// tree and reducing overhead.
guint kMaxTreeDepth = 512;

// Name of the file to store the final result.
gchar *kOutputFile = "halfempty.out";

// Name of the command to run.
gchar *kCommandPath;

// Original input file.
gchar *kInputFile;

// If a thread is already processing a workunit, we could kill the task. This
// makes things faster, but could leave temporary files lying about.
gboolean kKillFailedWorkers = true;

// Maybe you want to be notified about a failure, so (for example) you can trap
// SIGUSR1 instead and then cleanup in your script.
gint kKillFailedWorkersSignal = SIGTERM;

// If a process takes longer than this, we will send it SIGALRM.
gint kMaxProcessTime = 0;

// If you want to debug halfempty, then I can generate a dot file you can
// browse in xdot.
gboolean kGenerateDotFile = false;

// For real workloads, dot files can get too big to render. I can simplify them
// by folding TASK_STATUS_DISCARDED branches.
gboolean kSimplifyDotFile = false;

// Rather than exit when all strategies are completed, halfempty can make
// random changes to try and escape local minima.
// We will still generate normal output, but then will keep trying to improve
// that until interrupted.
gboolean kContinueSearch = false;

// Sometimes simplifying a file can shake out new minimization paths, so we can
// re-run the bisection until the result is stable (i.e. doesn't change).
gboolean kIterateUntilStable = false;

// Increase for more debugging messages.
guint kVerbosity = 0;

// Minimize all informational messages, try to only print errors.
gboolean kQuiet = false;

// Verify the input task is sane.
gboolean kVerifyInput = true;

// Help to debug synchronization problems by sleeping before exec().
guint kSleepSeconds = 0;

// Silence stdout/stderr.
gboolean kSilenceChildStdout = true;
gboolean kSilenceChildStderr = true;

// The rlimits we set in the child process, which can be configured via the
// commandline with --limit.
struct rlimit kChildLimits[RLIMIT_NLIMITS];

// Monitor mode opens xdot and displays pretty graphs while minimizing.
gboolean kMonitorMode = false;
gchar   *kMonitorTmpImageFilename;
gchar   *kMonitorTmpHtmlFilename;

// TODO:
//  Skip strategies.
