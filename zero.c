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

#define G_LOG_DOMAIN "zero"
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/sendfile.h>
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


// The structure of our user data.
typedef struct {
    size_t  offset;
    size_t  chunksize;
} bisect_t;

// Configurable Knobs.
static gchar kZeroCharacter = 0;

static const GOptionEntry kZeroOptions[] = {
    { "zero-char", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &kZeroCharacter, "Use this byte value when simplifying (0-255) (default=0).", "byte" },
    { NULL },
};

static const gchar kDescription[] =
    "Zero consecutively larger chunks of data from the file";

// Create a new node derived from the parent that can be inserted into our
// binary tree. node is a pointer to the current leaf, which we need to prepare
// a child for. i.e. node will be our parent.
// In this code there is the concept of a "parent" and a "source".
//  * parent is the node immediately above us in the tree, we use this to
//    determine parameters like what offset we're at, our offset will be
//    parent->offset += increment or whatever.
//  * source is the previous *successful* node in the tree, where we get our
//    data from. Parent cannot be the source unless it was successful, because it
//    might have had data removed we need.
static task_t * strategy_zero_data(GNode *node)
{
    task_t *child  = NULL;              // The new task we're about to return.
    task_t *parent = node->data;        // The task above us in the tree.
    task_t *source = parent;            // Where we get our data from.
    guint   adjust = 0;                 // How many times we tried to fit a chunk.

    // We dont hold the lock on parent, but user data will never change.
    bisect_t *parentstatus  = parent->user;
    bisect_t *childstatus   = g_new0(bisect_t, 1);

    g_debug("strategy_bisect_data(%p)", node);

    // If this is the root node, we're being called to initialize a new tree.
    if (parentstatus == NULL && G_NODE_IS_ROOT(node)) {
        g_debug("initializing a new root node %p, size %lu", node, parent->size);

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

    // Check if we've finished a chunksize.
    if (parentstatus->offset + parentstatus->chunksize > parent->size) {
        g_info("reached end of cycle (offset %lu + chunksize %lu > size %lu), starting next cycle",
               parentstatus->offset,
               childstatus->chunksize,
               parent->size);

        childstatus->offset       = 0;
        childstatus->chunksize  >>= 1;
    } else {
        g_debug("incrementing offset %lu => %lu",
                childstatus->offset,
                childstatus->offset + childstatus->chunksize);
        // TODO: what if offset now >= size?
        childstatus->offset += childstatus->chunksize;
    }

    // Check if this is the end of a cycle.
    if (childstatus->chunksize == 0) {
        g_info("final cycle complete, cannot start a new cycle");
        goto nochild;
    }
  restart:

    // Here is the problem, it's pointless trying to zero out chunks we've
    // already zeroed out. This means we need to start at the root, and see if
    // our offset + chunksize is already inside a SUCCESS node (dont care about
    // FAIL, because we're smaller).
    for (GNode *current = node; !G_NODE_IS_ROOT(current); current = current->parent) {
        gboolean adjusted = false;
        task_t  *currtask = current->data;

        g_assert_nonnull(currtask);
        g_assert_nonnull(currtask->user);

        if (currtask->status == TASK_STATUS_SUCCESS) {
            bisect_t *b = currtask->user;

            // An ancestor cannot possibly have a smaller chunksize.
            g_assert_cmpint(childstatus->chunksize, <=, b->chunksize);

            while (childstatus->offset >= b->offset && childstatus->offset + childstatus->chunksize <= b->offset + b->chunksize) {
                adjusted = true;
                adjust++;
                g_debug("offset %lu (chunksize %lu) already encapsulated, trying next offset", childstatus->offset, childstatus->chunksize);

                if ((childstatus->offset += childstatus->chunksize) > parent->size) {
                    g_debug("adjustment caused a new cycle to start, new chunksize %lu", childstatus->chunksize >> 1);

                    childstatus->offset = 0;
                    childstatus->chunksize >>= 1;

                    if (childstatus->chunksize == 0) {
                        g_info("final cycle complete, cannot start a new cycle");
                        goto nochild;
                    }
                }
            }

            if (adjusted) {
                goto restart;
            }
        }
    }

    g_info("made %u offset adjustments scanning tree, final offset: %lu, chunksize: %lu", adjust, childstatus->offset, childstatus->chunksize);

    // Traverse up the tree to find the first SUCCESS node, we base our data on that.
    if (source->status != TASK_STATUS_SUCCESS) {
        for (GNode *current = node; current; current = current->parent) {
            source = current->data;
            if (source->status == TASK_STATUS_SUCCESS) {
                break;
            }
        }

        // The root node has TASK_STATUS_SUCCESS, so this is impossible.
        g_assert(source);
    }

    // OK, looks like we've never tried zeroing this chunk before.
    // What if it is already zero though, it's pointless trying it again.
    gpointer b1 = g_malloc0(childstatus->chunksize);
    gpointer b2 = g_malloc0(childstatus->chunksize);
    gssize count = pread(source->fd, b1, childstatus->chunksize, childstatus->offset);

    if (kZeroCharacter != 0) {
        memset(b2, kZeroCharacter, childstatus->chunksize);
    }

    if (count != childstatus->chunksize) {
        g_info("%ld != %lu (offset %lu, size %lu, chunksize %lu)", count, childstatus->chunksize, childstatus->offset, parent->size, childstatus->chunksize);
        if (count < 0) {
            g_assert(false);
        }
    }
    if (memcmp(b1, b2, childstatus->chunksize) == 0) {
        g_info("no need to test this guy, he was already all %#02x", kZeroCharacter);
        g_free(b1);
        g_free(b2);

        if ((childstatus->offset += childstatus->chunksize) > parent->size) {
            g_debug("adjustment caused a new cycle to start, new chunksize %lu", childstatus->chunksize >> 1);

            childstatus->offset = 0;
            childstatus->chunksize >>= 1;

            if (childstatus->chunksize == 0) {
                g_info("final cycle complete, cannot start a new cycle");
                goto nochild;
            }
        }

        goto restart;
    }
    g_free(b1);
    g_free(b2);


    // OK, we need this guy, acquire the lock.
    g_mutex_lock(&source->mutex);

    // If it's success, the fd must be open and valid.
    g_assert_cmpint(source->fd, !=, -1);

    // This cannot possible be wrong.
    g_assert_cmpuint(source->size, ==, g_file_size(source->fd));

    // OK, we can do a bisection now.
    child->fd = g_unlinked_tmp(NULL);

    // Size should never change for this strategy.
    child->size = source->size;

    // i didnt think this was possible because how can child be smaller than an ancestor?
    if (childstatus->offset > source->size)
        goto nochildunlock;

    if (g_sendfile_all(child->fd, source->fd, 0, childstatus->offset) == false) {
        g_warning("sendfile failed while trying to construct new file");
        g_assert(false);
        goto nochildunlock;
    }

    if (kZeroCharacter == '\0') {
        // Insert some nuls, ftruncate() will do this for free.
        ftruncate(child->fd, MIN(source->size, childstatus->offset + childstatus->chunksize));
    } else {
        gchar *buf = g_malloc(MIN(source->size - childstatus->offset, childstatus->chunksize));
        memset(buf, kZeroCharacter, MIN(source->size - childstatus->offset, childstatus->chunksize));
        write(child->fd, buf, MIN(source->size - childstatus->offset, childstatus->chunksize));
        g_free(buf);
    }

    // ftruncate does not update offset.
    lseek(child->fd, 0, SEEK_END);

    if (childstatus->offset + childstatus->chunksize <= source->size) {
        if (g_sendfile_all(child->fd,
                           source->fd,
                           childstatus->offset + childstatus->chunksize,
                           source->size - childstatus->chunksize - childstatus->offset) == false) {
            g_warning("sendfile failed while trying to construct new file");
            g_assert(false);
            goto nochildunlock;
        }
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
REGISTER_STRATEGY(zero, kDescription, kZeroOptions, strategy_zero_data);
