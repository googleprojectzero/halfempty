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

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <glib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "flags.h"
#include "limits.h"

// This file is part of halfempty - a fast, parallel testcase minimization tool.
//
// Parse and setup rlimits for child processes.
//

static const gchar * limit_to_str(uint8_t limit)
{
    static const gchar * limits[UINT8_MAX] = {
        [RLIMIT_CPU]        "RLIMIT_CPU",
        [RLIMIT_FSIZE]      "RLIMIT_FSIZE",
        [RLIMIT_DATA]       "RLIMIT_DATA",
        [RLIMIT_STACK]      "RLIMIT_STACK",
        [RLIMIT_CORE]       "RLIMIT_CORE",
        [RLIMIT_RSS]        "RLIMIT_RSS",
        [RLIMIT_NOFILE]     "RLIMIT_NOFILE",
        [RLIMIT_AS]         "RLIMIT_AS",
        [RLIMIT_NPROC]      "RLIMIT_NPROC",
        [RLIMIT_MEMLOCK]    "RLIMIT_MEMLOCK",
        [RLIMIT_LOCKS]      "RLIMIT_LOCKS",
        [RLIMIT_SIGPENDING] "RLIMIT_SIGPENDING",
        [RLIMIT_MSGQUEUE]   "RLIMIT_MSGQUEUE",
        [RLIMIT_NICE]       "RLIMIT_NICE",
        [RLIMIT_RTPRIO]     "RLIMIT_RTPRIO",
        [RLIMIT_RTTIME]     "RLIMIT_RTTIME",
    };

    return limits[limit];
}

static gint str_to_limit(const gchar *limit)
{
    guint i;
    for (i = 0; i < RLIMIT_NLIMITS; i++) {
        if (limit_to_str(i) && strcmp(limit, limit_to_str(i)) == 0)
            return i;
    }

    return -1;
}

gboolean decode_proc_limit(const gchar *option_name,
                           const gchar *value,
                           gpointer data,
                           GError **error)
{
    gint resource;
    gint limit;
    gchar **param;

    g_assert_cmpstr(option_name, ==, "--limit");
    g_assert_nonnull(value);

    // value will be of them form RLIMIT_FOO=12345.
    param = g_strsplit(value, "=", 2);

    // Check we got two parameters.
    if (param[0] == NULL || param[1] == NULL) {
        g_warning("You passed the string %s to %s, but that is not a valid limit specification",
                  value,
                  option_name);
        g_warning("See setrlimit(3) manual for a full list, for example, --limit RLIMIT_CPU=120");
        g_strfreev(param);
        return false;
    }

    // Decode those two options.
    resource    = str_to_limit(param[0]);
    limit       = g_ascii_strtoll(param[1], NULL, 0);

    if (resource == -1) {
        g_warning("You passed the string %s to %s, but `%s` is not recognized as a limit name",
                  value,
                  option_name,
                  *param);
        g_warning("See setrlimit(3) manual for a full list, for example, --limit RLIMIT_CPU=120");
        g_strfreev(param);
        return false;
    }

    kChildLimits[resource].rlim_cur = limit;
    kChildLimits[resource].rlim_max = limit;

    g_strfreev(param);
    return true;
}

static void __attribute__((constructor)) init_child_limits()
{
    g_debug("configuring default rlimits for child process");

    for (gint i = 0; i < RLIMIT_NLIMITS; i++) {
        if (getrlimit(i, &kChildLimits[i]) != 0) {
            g_warning("failed to getrlimit for %u, %m", i);
        }

        g_debug("Configured rlimit %s => { %ld, %ld }",
                limit_to_str(i),
                kChildLimits[i].rlim_cur,
                kChildLimits[i].rlim_max);
    }


    // OK, but lets set some sane defaults.
    kChildLimits[RLIMIT_CORE].rlim_cur = 0;
    kChildLimits[RLIMIT_CORE].rlim_max = 0;
}

