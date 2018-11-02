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

#ifndef __FLAGS_H
#define __FLAGS_H

// This file is part of halfempty - a fast, parallel testcase minimization tool.

#include <sys/resource.h>

// Some OSes (e.g. macOS) define the number of RLIMITs under a different name.
#if !defined(RLIMIT_NLIMITS) && defined(RLIM_NLIMITS)
  #define RLIMIT_NLIMITS RLIM_NLIMITS
#endif

// See flags.c for documentation.
extern guint kMaxUnprocessed;
extern guint kCleanupThreads;
extern guint kProcessThreads;
extern guint kWorkerPollDelay;
extern gchar *kOutputFile;
extern gchar *kCommandPath;
extern gboolean kKillFailedWorkers;
extern gint kKillFailedWorkersSignal;
extern gint kMaxProcessTime;
extern gboolean kGenerateDotFile;
extern gboolean kSimplifyDotFile;
extern gboolean kContinueSearch;
extern gboolean kIterateUntilStable;
extern guint kVerbosity;
extern gboolean kQuiet;
extern gboolean kVerifyInput;
extern guint kSleepSeconds;
extern struct rlimit kChildLimits[RLIMIT_NLIMITS];
extern gchar *kInputFile;
extern gboolean kSilenceChildStdout;
extern gboolean kSilenceChildStderr;
extern gboolean kMonitorMode;
extern gchar *kMonitorTmpImageFilename;
extern gchar *kMonitorTmpHtmlFilename;


#else
# warning flags.h included twice
#endif
