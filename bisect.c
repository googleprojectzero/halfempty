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

#define G_LOG_DOMAIN "bisect"
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include "task.h"
#include "proc.h"
#include "util.h"
#include "tree.h"

// This file is part of halfempty - a fast, parallel testcase minimization tool.
//
// This is the main implementation of the bisection algorithm.
// We're passed a GNode where a new workunit is required, we can examine the
// state of the tree and traverse around it, and then produce a workunit.
//
// The algorithm works like this, every node has a { offset, chunksize } pair
// associated with it. We try removing a chunksize chunk of data from every
// offset until we reach the end of the file. If we do reach the end of the
// file, we half chunksize, and start again at offset zero.
//
// The major complication in the mechanics of this is that we need to know if
// the parent node succeeded or failed. If it succeeded then we just removed a
// chunk and don't need to increment offset. If it failed, then we need to make
// sure that chunk goes back.
//

// The structure of our user data.
typedef struct {
    size_t  offset;
    size_t  chunksize;
} bisect_t;

// Configurable Knobs.
static gboolean kSkipEmpty = false;
static gint kSkipThreshold = 0;

static const GOptionEntry kBisectOptions[] = {
    { "bisect-skip-empty", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
            &kSkipEmpty,
            "Don't try to test empty input.",
            NULL },
    { "bisect-skip-threshold", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_INT,
            &kSkipThreshold,
            "Skip truncated chunks smaller than this.",
            "bytes" },
    { NULL },
};

static const gchar kDescription[] =
    "Remove consecutively larger chunks of data from the file";

// In this code there is the concept of a "parent" and a "source".
//
//
//  * "parent" is the node immediately above us in the tree, we use this to
//    determine parameters like what offset we're at, our offset will be
//    parent->offset += increment.
//  * "source" is the previous *successful* node in the tree, where we get our
//    data from. Parent cannot be the source unless it was successful, because
//    it
//    might have had data removed we need.
//
// The source node could be some distance towards the root node.
//
// Generate a workunit for this position in the tree.
static task_t * strategy_bisect_data(GNode *node)
{
    task_t *child  = NULL;              // The new task we're about to return.
    task_t *parent = node->data;        // The task above us in the tree.
    task_t *source = parent;            // Where we get our data from.

    // We don't hold the lock on parent, but user data will never change.
    bisect_t *parentstatus  = parent->user;
    bisect_t *childstatus   = g_new0(bisect_t, 1);

    g_debug("strategy_bisect_data(%p)", node);

    // If this is the root node, we're being called to initialize a new tree.
    if (parentstatus == NULL && G_NODE_IS_ROOT(node)) {
        g_debug("initializing a new root node size %lu", parent->size);

        // If this was already set, then something has gone wrong.
        g_assert_cmpint(g_node_n_children(node), ==, 0);

        childstatus->offset     = 0;
        childstatus->chunksize  = parent->size;
        parent->user            = childstatus;
        return parent;
    }

    g_assert_nonnull(parentstatus);

    // Initialize child from parent.
    child           = g_new0(task_t, 1);
    child->fd       = -1;
    child->size     = parent->size;
    child->status   = TASK_STATUS_PENDING;
    child->user     = memcpy(childstatus, parentstatus, sizeof(bisect_t));

    // Check if we've finished a chunksize, which means we need to reset offset
    // to zero with a smaller chunksize. We continue until chunksize is zero.
    if (parentstatus->offset + parentstatus->chunksize > parent->size) {
        g_info("reached end of cycle (offset %lu + chunksize %lu > size %lu)",
               parentstatus->offset,
               childstatus->chunksize,
               parent->size);

        childstatus->offset       = 0;
        childstatus->chunksize  >>= 1;
    } else if (parent->status != TASK_STATUS_SUCCESS) {
        g_debug("parent failed or pending, trying next offset %lu => %lu",
                childstatus->offset,
                childstatus->offset + childstatus->chunksize);

        // *If* the parent succeeded, then we increment offset, otherwise we
        // don't need to.
        // TODO: what if offset now >= size?
        childstatus->offset += childstatus->chunksize;
    } else {
        g_debug("parent succeeded, not incrementing offset from %lu",
                childstatus->offset);
    }

    if (childstatus->chunksize == 0) {
        g_info("final cycle complete.");
        goto nochild;
    }

    // Traverse up the tree to find the first SUCCESS node, we base our data on
    // that.
    if (source->status != TASK_STATUS_SUCCESS) {
        for (GNode *current = node; current; current = current->parent) {
            source = current->data;
            if (source->status == TASK_STATUS_SUCCESS) {
                break;
            }
        }

        // The root node has TASK_STATUS_SUCCESS, so it is impossible that we
        // can't find a node with TASK_STATUS_SUCCESS unless the tree is
        // corrupt.
        g_assert(source);
    }

    // The source could be empty if the empty file worked, just give up I
    // guess?
    if (source->size == 0) {
        g_info("empty file succeeded, no further reduction possible");
        goto nochild;
    }

    g_debug("creating task for %p with parent %p and source %p",
            node,
            parent,
            source);

    // OK, we need to access this fd, so acquire the lock.
    g_mutex_lock(&source->mutex);

    // If it's success, the fd must be open and valid.
    g_assert_cmpint(source->fd, !=, -1);

    // This cannot possibly be wrong.
    g_assert_cmpuint(source->size, ==, g_file_size(source->fd));

    // OK, we can do a bisection now.
    child->fd = g_unlinked_tmp(NULL);

    // I don't think this is possible.
    if (childstatus->offset > source->size)
        goto nochildunlock;

    // Initialize the new child with everything up to offset.
    if (g_sendfile_all(child->fd,
                       source->fd,
                       0,
                       childstatus->offset) == false) {
        g_error("sendfile failed while trying to construct new file, %m");
        goto nochildunlock;
    }

    child->size = childstatus->offset;

    if (childstatus->offset + childstatus->chunksize <= source->size) {
        if (g_sendfile_all(child->fd,
                           source->fd,
                           childstatus->offset + childstatus->chunksize,
                           source->size
                                - childstatus->chunksize
                                - childstatus->offset) == false) {
            g_error("sendfile failed while trying to construct new file, %m");
            goto nochildunlock;
        }

        child->size += source->size
                        - childstatus->chunksize
                        - childstatus->offset;
    }

    g_assert_cmpuint(child->size, ==, g_file_size(child->fd));

    // Finished with source object.
    g_mutex_unlock(&source->mutex);

    return child;

  nochildunlock:
    g_mutex_unlock(&source->mutex);

  nochild:
    if (child) {
        close(child->fd);
    }
    g_free(child);
    g_free(childstatus);
    return NULL;
}

// Add this strategy to the global list.
REGISTER_STRATEGY(bisect, kDescription, kBisectOptions, strategy_bisect_data);
