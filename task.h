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

#ifndef __TASK_H
#define __TASK_H

typedef enum {
    TASK_STATUS_SUCCESS,        // The task was executed and succeeded.
    TASK_STATUS_FAILURE,        // The task was executed and failed.
    TASK_STATUS_PENDING,        // The task is in the execution queue.
    TASK_STATUS_DISCARDED,      // The task was pending, but got cancelled.
} status_t;

typedef struct {
    gint        fd;         // Data for this node, or -1 if none. rw lock required.
    gsize       size;       // Size of this data. rw lock required.
    gpointer    user;       // Strategy-specific context. rw lock required.
    status_t    status;     // Task status (completed, pending, etc). atomic rw required.
    GMutex      mutex;      // Mutex.
    GTimer     *timer;      // Used to calculate total compute time.
    GPid        childpid;   // pid of active task, if applicable.
} task_t;

static inline const gchar * string_from_status(status_t status)
{
    const gchar * names[] = {
        [TASK_STATUS_SUCCESS]   = "TASK_STATUS_SUCCESS",
        [TASK_STATUS_FAILURE]   = "TASK_STATUS_FAILURE",
        [TASK_STATUS_PENDING]   = "TASK_STATUS_PENDING",
        [TASK_STATUS_DISCARDED] = "TASK_STATUS_DISCARDED",
    };

    return names[status];
}

extern gchar *testscript;

typedef task_t * (* strategy_cb_t)(GNode *);

#else
# warning task.h included twice
#endif
