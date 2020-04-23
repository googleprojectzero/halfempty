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
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include "task.h"
#include "proc.h"
#include "util.h"
#include "tree.h"
#include "flags.h"

// This file is part of halfempty - a fast, parallel testcase minimization tool.
//
// Tree management and strategy driver routines.
//

static void duplicate_final_node(gint *fd);
static void show_tree_statistics(void);
static void cleanup_tree(void);
static gboolean root_path_finalized(GNode *node);
static GNode * find_finalized_node(GNode *root, gboolean success);
static gdouble path_total_elapsed(GNode *node);
static gint print_status_message(GTimer *elapsed, gint finaldepth);
static gint collapse_finalized_failure_paths(void);

// This binary tree represents our path through the testcases we've generated
// so far. The root node contains the original input (although we may have
// thrown away the data as we no longer need it, but the task will remain).
//
// To iterate through the tree, you choose whether you want the success branch
//
// curr = g_node_success(tree);
//   or
// curr = g_node_failure(tree);
//
// Nodes are never removed from the tree, but new ones may be added, and
// existing nodes may change (but they will be locked).
//

static GNode *tree;
static GNode *retired;
static GMutex treelock;
static GCond treecond;
static GThreadPool *threadpool;
static GThreadPool *cleanup;
static gdouble collapsedtime;

gint kNumStrategies;
strategy_t kStrategyList[MAX_STRATEGIES];

// This is the main driver that manages the bisection tree and calls the
// strategy callbacks. It waits for workunits to complete, and then fills up
// the queue again.
gboolean build_bisection_tree(gint fd,
                              strategy_cb_t callback,
                              gint *outfd,
                              gulong flags)
{
    gint finaldepth;
    gint backoff;
    task_t *root;
    GTimer *elapsed;

    // Initialize threadpool workers, each one simply executes a testcase and
    // updates the tree with the result.
    threadpool = g_thread_pool_new((GFunc) process_execute_jobs,
                                   NULL,
                                   kProcessThreads,
                                   TRUE,
                                   NULL);

    // This threadpool just cleans up tasks and mostly just waits on locks.
    cleanup = g_thread_pool_new((GFunc) cleanup_orphaned_tasks,
                                NULL,
                                kCleanupThreads,
                                FALSE,
                                NULL);

    backoff         = 0;
    finaldepth      = 0;
    root            = g_new0(task_t, 1);
    tree            = g_node_new(root);
    retired         = g_node_new(NULL);
    root->fd        = fd;
    root->size      = g_file_size(fd);
    root->status    = TASK_STATUS_PENDING;
    elapsed         = g_timer_new();

    // Verify the input task is sane.
    if (kVerifyInput) {
        g_message("Verifying the original input executes successfully... (skip with --noverify)");
        process_execute_jobs(tree);
        if (root->status != TASK_STATUS_SUCCESS) {
            g_message("This program expected `%s` to return successfully",
                      kCommandPath);
            g_message("for the original input (i.e. exitcode zero).");
            g_message("Try it yourself to verify it's working.");
            
            // See the FAQ for why cat is used and not redirection.
            g_message("Use a command like: `cat %s | %s || echo failed`",
                      kInputFile,
                      kCommandPath);
            return false;
        } else {
            g_message("The original input file succeeded after %.1f seconds.",
                    g_timer_elapsed(root->timer, NULL));
        }
    } else {
        // Just fake it.
        root->status = TASK_STATUS_SUCCESS;
        root->timer  = g_timer_new();
        g_timer_stop(root->timer);
    }

    // Initialize the root node
    callback(tree);

    // Keep track of time taken.
    g_timer_reset(elapsed);

    while (true) {
        GNode *current = tree;

        // Take the treelock so we can modify the tree.
        g_mutex_lock(&treelock);

        // Don't generate too much work or we'll explore too far down a wrong
        // path.
        // This condition is always signaled when a workunit completes.
        while (g_thread_pool_unprocessed(threadpool) > kMaxUnprocessed)
            g_cond_wait_until(&treecond,
                              &treelock,
                              g_get_monotonic_time() + kMaxWaitTime);

        // Now that we have the lock, the tree is stable until we release it.
        g_debug("generator thread obtained treelock, finding next leaf");

        // Print statistics on current tree state.
        // Note that "finalized" means that the task itself and every node
        // along it's path to the root is complete (i.e. not pending).
        finaldepth = print_status_message(elapsed, finaldepth);

        // We can collapse exceptionally long trees so that we're not wasting
        // valuable cycles traversing linked lists. Note that we never delete a
        // success node, but dont care about failure nodes.
        if (g_node_max_height(tree) > kMaxTreeDepth)
            finaldepth = collapse_finalized_failure_paths();

        // Scan for the next location to insert work.
        // The idea is this, from the root:
        //      for (node = root; node != leaf;) {
        //          if (node->status == SUCCESS)
        //              node = g_node_success(node);
        //          if (node->status == FAILURE)
        //              node = g_node_failure(node);
        //      }
        //      add_new_work_here(node); // node must be a leaf node
        for (gint depth = 0;; depth++) {
            task_t *currtask = current->data;

            // If there is no task, this must be an empty placeholder from
            // g_node_new(NULL) below. It turns out we do need this, so just
            // replace it with a real workunit.
            if (currtask == NULL) {
                current->data = callback(current->parent);

                // I use depth to indent the messages so you can see the
                // progress.
                g_debug("%*sfound a NULL task pointer, generating task",
                        depth,
                        "");

                // Make sure we were able to generate a workunit for this node.
                if (current->data == NULL) {
                    // Looks like this is the end of the path.
                    g_debug("%*sno more work possible on this path", depth, "");

                    // So this was a placeholder node that we did need, but
                    // couldnt generate a workunit for it when it turned out we
                    // did need it.
                    // That means a path we didn't think was going to happen
                    // did happen, but we can't complete it.
                    //
                    // Two options:
                    //  1. We're finalized, then that must mean we're done.
                    //  2. We're not finalized, so just wait for some more work
                    //     and see if that finds another path.
                    if (root_path_finalized(current->parent) == true) {
                        goto finalized;
                    }
                    goto delay;
                }

                // That worked, submit the task.
                g_thread_pool_push(threadpool, current, NULL);
                break;
            }

            // We should never traverse into a discarded branch.
            g_assert_cmpint(currtask->status, !=, TASK_STATUS_DISCARDED);

            g_debug("%*sfound a %s task, size %lu ",
                    depth,
                    "",
                    string_from_status(currtask->status),
                    currtask->size);

            // If this is a leaf node, then we need to append a new task here.
            if (G_NODE_IS_LEAF(current)) {
                task_t *child = callback(current);

                g_debug("%*snode is a leaf node, generating children",
                        depth,
                        "");

                if (child == NULL) {
                    g_debug("%*sno more children possible", depth, "");
                    // We can't generate any more work, but that doesn't mean
                    // we're finished - there might be unprocessed work in the
                    // queue that changes our path through the tree.
                    if (root_path_finalized(current) == true) {
                        // The threadpool must be empty, because we've
                        // discarded everything else?
                        g_assert_cmpint(g_thread_pool_unprocessed(threadpool),
                                        ==,
                                        0);
                        goto finalized;
                    }
                    goto delay;
                }

                // Is the node above us already finalized and is successful? If
                // so, we know which route to take. otherwise, we just guess
                // it's going to fail.
                if (currtask->status == TASK_STATUS_SUCCESS) {
                    // Placeholder Failure node
                    g_node_insert(current, false, g_node_new(NULL));
                    // Success node
                    g_thread_pool_push(threadpool,
                                       g_node_insert(current,
                                                     true,
                                                     g_node_new(child)),
                                                     NULL);
                } else {
                    // Failure node
                    g_thread_pool_push(threadpool,
                                       g_node_insert(current,
                                                     false,
                                                     g_node_new(child)),
                                                     NULL);
                    // Placeholder Success node
                    g_node_insert(current, true, g_node_new(NULL));
                }

                // All done.
                break;
            }

            // The node is not a leaf, so we haven't found the right place to
            // insert work yet.
            g_debug("%*snode is not a leaf, traversing", depth, "");

            // This is not a leaf, so traverse
            if (currtask->status == TASK_STATUS_SUCCESS) {
                current = g_node_success(current);
            } else {
                current = g_node_failure(current);
            }
        }

        // I can generate a pretty graph so you can monitor the status.
        if (kMonitorMode) {
            generate_monitor_image(tree);
        }

        g_debug("generator thread releasing tree lock");
        g_mutex_unlock(&treelock);

        // Reset backoff counter.
        backoff = 0;
        continue;

    finalized:
        g_message("Reached the end of our path through tree, "
                "all nodes were finalized");

        // Unlock the tree and let threadpool workers finish.
        g_mutex_unlock(&treelock);
        g_thread_pool_free(threadpool, FALSE, TRUE);
        g_thread_pool_free(cleanup, FALSE, TRUE);

        // Cleanup and produce output.
        show_tree_statistics();
        duplicate_final_node(outfd);
        cleanup_tree();
        g_timer_destroy(elapsed);
        return true;

    delay:
        g_debug("generator thread releasing tree lock (delayed, ctr %u)",
                 backoff);
        g_mutex_unlock(&treelock);
        g_usleep(kWorkerPollDelay * ++backoff);
        continue;
    }

    g_assert_not_reached();
    return false;
}

// This routine cleans up tasks that are on discarded branches.
// This is the only location that tasks are destroyed and should only be called
// from the gc thread.
void cleanup_orphaned_tasks(task_t *task)
{
    GPid childpid = task->childpid;

    g_assert(task);

    // If requested, aggressively try to cleanup discarded tasks.
    if (kKillFailedWorkers && childpid > 0) {
        kill(-childpid, kKillFailedWorkersSignal);
    }

    g_debug("thread %p cleaning up task %p (pid=%d), now attempting to lock",
            g_thread_self(),
            task,
            task->childpid);

    g_mutex_lock(&task->mutex);

    g_debug("thread %p acquired lock on task %p, state %s",
            g_thread_self(),
            task,
            string_from_status(task->status));

    // Ensure pending tasks dont get executed. 
    if (task->status == TASK_STATUS_PENDING)
        task->status = TASK_STATUS_DISCARDED;

    // We hold the lock on this task now, so can clean up the file descriptor
    // and zombie.
    g_close(task->fd, NULL);

    if (task->childpid > 0) {
        if (waitpid(task->childpid, NULL, WNOHANG) != task->childpid) {
            g_critical("waitpid() didn't return immediately with zombie, this shouldn't happen");
        }
    }

    task->fd       = -1;
    task->childpid = 0;

    // Nothing else we need to do, unlock.
    g_mutex_unlock(&task->mutex);

    g_debug("task %p unlocked by %p, now discarded", task, g_thread_self());
}

static gboolean abort_task_helper(GNode *node, gpointer data)
{
    // We can't lock tasks here or we would deadlock, so push them on a
    // queue to cleanup later.
    if (node->data) {
        g_thread_pool_push(cleanup, node->data, NULL);
    }
    return false;
}

void abort_pending_tasks(GNode *root)
{
    if (root == NULL) {
        g_debug("abort_pending_tasks() called, but no child nodes to traverse");
        return;
    }

    // Prevent any new jobs from being inserted.
    g_mutex_lock(&treelock);

    g_node_traverse(root,
                    G_PRE_ORDER,
                    G_TRAVERSE_ALL,
                    -1,
                    abort_task_helper,
                    NULL);

    // Let work continue.
    g_mutex_unlock(&treelock);
}

// Is the path from this node to the root node finalized or pending?
// XXX: Must hold treelock.
static gboolean root_path_finalized(GNode *node)
{
    for (; !G_NODE_IS_ROOT(node); node = node->parent) {
        task_t *task = node->data;

        g_assert_nonnull(task);

        if (task->status != TASK_STATUS_SUCCESS
         && task->status != TASK_STATUS_FAILURE)
            return false;
    }

    g_assert(G_NODE_IS_ROOT(node));
    return true;
}

void process_execute_jobs(GNode *node)
{
    task_t *task = node->data;
    gint result;

    g_assert(task);
    g_mutex_lock(&task->mutex);

    // Note that other threads can examine this task, but cannot modify it
    // while locked. It is not permitted to use the file descriptor without
    // holding the lock.
    g_debug("thread %p processing task %p, size %lu, fd %d, status %s",
            g_thread_self(),
            task,
            task->size,
            task->fd,
            string_from_status(task->status));

    // Check before we start the task.
    if (task->status == TASK_STATUS_DISCARDED) {
        g_debug("task %p was discarded, nothing left to do", task);
        g_mutex_unlock(&task->mutex);
        return;
    }

    // The only two possibilities are discarded and pending.
    g_assert_cmpint(task->status, ==, TASK_STATUS_PENDING);
    g_assert(task->timer == NULL);

    // Keep track of time elapsed;
    task->timer = g_timer_new();

    // Spawn a process to find result.
    result = submit_data_subprocess(task->fd, task->size, &task->childpid);

    // Count elapsed time.
    g_timer_stop(task->timer);

    g_debug("thread %p, child returned %d after %.3f seconds, size %lu",
            g_thread_self(),
            result,
            g_timer_elapsed(task->timer, NULL),
            task->size);

    g_assert_cmpint(task->childpid, !=, 0);

    switch (result) {
        case  0: g_debug("task %p success, aborting mispredicted jobs", task);

                 // Update status.
                 task->status = TASK_STATUS_SUCCESS;

                 // We don't need to hold the lock anymore.
                 g_mutex_unlock(&task->mutex);

                 // Any tasks on the failure branch were mispredicted.
                 abort_pending_tasks(g_node_failure(node));

                 // Print status message
                 g_info("thread %p found task %p succeeded after %.3f seconds, size %lu, depth %d",
                        g_thread_self(),
                        task,
                        g_timer_elapsed(task->timer, NULL),
                        task->size,
                        g_node_depth(node));

                 break;
                 // All non-zero exit codes and failures are discarded.
        default: g_debug("unexpected result %d from task %p", result, task);
                 // fallthrough
        case  1: g_debug("task %p failed, fd %d, pid %d",
                         task,
                         task->fd,
                         task->childpid);

                 g_assert_cmpint(task->status, ==, TASK_STATUS_PENDING);

                 // Update status.
                 task->status = TASK_STATUS_FAILURE;

                 // We now know for sure we dont need it, so we can release
                 // these resources.
                 g_thread_pool_push(cleanup, task, NULL);

                 // All done.
                 g_mutex_unlock(&task->mutex);
                 break;
    }

    g_debug("thread %p completed workunit %p", g_thread_self(), task);

    g_cond_signal(&treecond);
    return;
}

// Count all the timers from here to root.
// XXX: must hold tree lock
static gdouble path_total_elapsed(GNode *node)
{
    gdouble elapsed = 0;

    // Don't call me on random nodes, must be finalized.
    g_assert(root_path_finalized(node));

    for (; !G_NODE_IS_ROOT(node); node = node->parent) {
        task_t *task = node->data;

        g_assert_nonnull(task);

        elapsed += g_timer_elapsed(task->timer, NULL);
    }

    g_assert(G_NODE_IS_ROOT(node));
    return elapsed;
}

struct tree_stats {
    gint failure;
    gint success;
    gint discarded;
    gdouble elapsed;
};

static gboolean analyze_tree_helper(GNode *node, gpointer user)
{
    struct tree_stats *stats = user;
    task_t *task = node->data;

    if (task == NULL) {
        return false;
    }

    g_assert_cmpint(task->status, !=, TASK_STATUS_PENDING);

    // Keep track of total compute time.
    if (task->status != TASK_STATUS_DISCARDED) {
        stats->elapsed += g_timer_elapsed(task->timer, NULL);
    }

    if (task->status == TASK_STATUS_SUCCESS) {
        stats->success++;
    } else if (task->status == TASK_STATUS_FAILURE) {
        stats->failure++;
    } else if (task->status == TASK_STATUS_DISCARDED) {
        stats->discarded++;
    } else {
        g_assert_not_reached();
    }

    return false;
}

static void show_tree_statistics(void)
{
    struct tree_stats stats = {
        .failure = 0,
        .success = 0,
        .discarded = 0,
        .elapsed = 0,
    };

    g_mutex_lock(&treelock);

    g_info("Analyzing tree treesize=%u, height=%u",
           g_node_n_nodes(tree, G_TRAVERSE_ALL),
           g_node_max_height(tree));

    if (kGenerateDotFile) {
        gchar dotfile[] = "finaltree.XXXXXX.dot";

        // I don't want the descriptor, just the filename.
        g_close(g_mkstemp(dotfile), NULL);

        g_message("Generating DOT file of final tree to %s (view it with xdot)...",
                dotfile);

        generate_dot_tree(tree, dotfile);
    }

    // Visit every node
    g_node_traverse(tree,
                    G_IN_ORDER,
                    G_TRAVERSE_ALL,
                    -1,
                    analyze_tree_helper,
                    &stats);
    g_node_traverse(retired,
                    G_IN_ORDER,
                    G_TRAVERSE_ALL,
                    -1,
                    analyze_tree_helper,
                    &stats);

    g_message("%u nodes failed, %u worked, %u discarded, %u collapsed",
            stats.failure,
            stats.success,
            stats.discarded,
            g_node_n_nodes(retired, G_TRAVERSE_ALL));
    g_message("%0.3f seconds of compute was required for final path",
            stats.elapsed);

    g_mutex_unlock(&treelock);

    return;
}

// Find the deepest finalized node, optionally with TASK_STATUS_SUCCESS
// XXX: Must hold treelock.
static GNode * find_finalized_node(GNode *root, gboolean success)
{
    GNode  *final   = NULL;
    task_t *task    = root->data;

    // Determine if the root node qualifies as finalized
    if (task == NULL) {
        g_debug("find_finalized_node(%p) -> root node was not finalized", root);
        return final;
    }

    if (task->status == TASK_STATUS_SUCCESS)
        final = root;
    if (!success && task->status == TASK_STATUS_FAILURE)
        final = root;

    while (!G_NODE_IS_LEAF(root)) {
        task = root->data;

        if (task == NULL)
            break;

        if (task->status == TASK_STATUS_SUCCESS) {
            final = root;
            root  = g_node_success(root);
        } else if (task->status == TASK_STATUS_FAILURE) {
            final = success ? final : root;
            root  = g_node_failure(root);
        } else {
            break;
        }
    }

    // Verify that looks sane.
    if (final) {
        task = final->data;
        g_assert_nonnull(task);
        g_assert_cmpint(task->status, !=, TASK_STATUS_PENDING);
        g_assert_cmpint(task->status, !=, TASK_STATUS_DISCARDED);
    }

    return final;
}

// This routine will dup() the filedescriptor for the final node with status
// TASK_STATUS_SUCCESS.
static void duplicate_final_node(gint *fd)
{
    GNode *success;
    task_t *task;

    g_mutex_lock(&treelock);

    success = find_finalized_node(tree, true);

    g_assert_nonnull(success);
    g_assert_nonnull(success->data);

    task = success->data;

    g_mutex_lock(&task->mutex);
    g_assert_cmpint(task->status, ==, TASK_STATUS_SUCCESS);
    g_assert_cmpint(task->fd, !=, -1);

    *fd = dup(task->fd);

    g_assert_cmpint(*fd, !=, -1);
    g_mutex_unlock(&task->mutex);
    g_mutex_unlock(&treelock);
    return;
}

// This should only be called when the tree is being destroyed, otherwise use
// the gc thread.
static gboolean cleanup_tree_helper(GNode *node, gpointer user)
{
    task_t *task = node->data;

    if (task == NULL)
        return false;

    g_debug("cleanup task %p, fd: %d", task, task->fd);

    cleanup_orphaned_tasks(task);

    g_free(task->user);

    if (task->timer) {
        g_timer_destroy(task->timer);
    }

    g_free(task);
    return false;
}

static void cleanup_tree(void)
{
    g_mutex_lock(&treelock);

    g_debug("cleanup_tree() acquired lock, about to free all resources");

    // Visit every node
    g_node_traverse(tree,
                    G_IN_ORDER,
                    G_TRAVERSE_ALL,
                    -1,
                    cleanup_tree_helper,
                    NULL);
    g_node_traverse(retired,
                    G_IN_ORDER,
                    G_TRAVERSE_ALL,
                    -1,
                    cleanup_tree_helper,
                    NULL);

    // Destroy tree
    g_node_destroy(tree);
    g_node_destroy(retired);

    g_debug("cleanup_tree() complete");

    // Unlock, it can now be used again.
    g_mutex_unlock(&treelock);

    return;
}

// This routine will collapse long paths of consecutive failures to
// compress very large trees. This should be rarely necessary.
// Returns the new distance from the root to the finalized success node.
// XXX: must hold tree lock.
static gint collapse_finalized_failure_paths(void)
{
    GNode *finalsuccess;
    GNode *finalnode;
    task_t *task;

    finalsuccess = find_finalized_node(tree, true);

    // There must always be at least one success node.
    g_assert_nonnull(finalsuccess);

    // Find the final success node, and move it up right up to the root.
    // All the others are transferred to the retired tree for cleanup.
    // Makre sure finalsuccess is not the root node, and not already the first
    // node where we would put it anyway.
    if (finalsuccess != tree && g_node_success(tree) != finalsuccess) {
        GNode *head = g_node_success(tree);
        GNode *tail = finalsuccess->parent;

        g_assert(g_node_is_ancestor(tree, head) == TRUE);
        g_assert(g_node_is_ancestor(tree, finalsuccess) == TRUE);
        g_assert(g_node_is_ancestor(head, finalsuccess) == TRUE);

        g_node_unlink(head);

        g_assert(g_node_is_ancestor(tree, head) == FALSE);
        g_assert(g_node_is_ancestor(tree, finalsuccess) == FALSE);
        g_assert(g_node_is_ancestor(head, finalsuccess) == TRUE);
        g_assert(g_node_is_ancestor(tail, finalsuccess) == TRUE);

        g_node_unlink(finalsuccess);

        g_assert(g_node_is_ancestor(tree, head) == FALSE);
        g_assert(g_node_is_ancestor(tree, finalsuccess) == FALSE);
        g_assert(g_node_is_ancestor(head, finalsuccess) == FALSE);
        g_assert(g_node_is_ancestor(tail, finalsuccess) == FALSE);

        g_node_insert(tree, TRUE, finalsuccess);

        g_assert(g_node_is_ancestor(tree, head) == FALSE);
        g_assert(g_node_is_ancestor(tree, finalsuccess) == TRUE);
        g_assert(g_node_is_ancestor(head, finalsuccess) == FALSE);

        // Keep track of how much time we're collapsing.
        collapsedtime += path_total_elapsed(tail);

        // Cleanup all tasks on this retired tree.
        g_node_traverse(head,
                        G_PRE_ORDER,
                        G_TRAVERSE_ALL,
                        -1,
                        abort_task_helper,
                        NULL);

        // Put it in the retired tree for cleanup by cleanup_tree()
        g_node_insert(retired, -1, head);
    }

    // Note that this returns the final node (regardless of success/fail).
    finalnode = find_finalized_node(tree, false);

    // There must always be at least one node.
    g_assert_nonnull(finalnode);
    g_assert_nonnull(finalnode->data);

    // Check this node is not already in place.
    if (finalsuccess != finalnode
     && g_node_success(finalsuccess) != finalnode
     && g_node_success(finalsuccess) != finalnode->parent) {
        GNode *head = g_node_success(finalsuccess);
        GNode *tail = finalnode->parent;

        g_assert(g_node_is_ancestor(tree, finalnode) == TRUE);
        g_assert(g_node_is_ancestor(finalsuccess, finalnode) == TRUE);
        g_assert(g_node_is_ancestor(finalsuccess, head) == TRUE);
        g_assert(g_node_is_ancestor(finalsuccess, tail) == TRUE);
        g_assert(g_node_is_ancestor(tail, finalnode) == TRUE);

        g_node_unlink(head);

        g_assert(g_node_is_ancestor(tree, finalnode) == FALSE);
        g_assert(g_node_is_ancestor(finalsuccess, finalnode) == FALSE);
        g_assert(g_node_is_ancestor(finalsuccess, head) == FALSE);
        g_assert(g_node_is_ancestor(finalsuccess, tail) == FALSE);
        g_assert(g_node_is_ancestor(tail, finalnode) == TRUE);

        g_node_unlink(finalnode);

        g_assert(g_node_is_ancestor(tree, finalnode) == FALSE);
        g_assert(g_node_is_ancestor(finalsuccess, finalnode) == FALSE);
        g_assert(g_node_is_ancestor(finalsuccess, head) == FALSE);
        g_assert(g_node_is_ancestor(finalsuccess, tail) == FALSE);
        g_assert(g_node_is_ancestor(tail, finalnode) == FALSE);

        // It's either root (must be success), or final success.
        task = finalsuccess->data;
        g_assert_cmpint(task->status, ==, TASK_STATUS_SUCCESS);

        g_node_insert(finalsuccess, TRUE, finalnode);

        g_assert(g_node_is_ancestor(tree, finalnode) == TRUE);
        g_assert(g_node_is_ancestor(finalsuccess, finalnode) == TRUE);
        g_assert(g_node_is_ancestor(finalsuccess, head) == FALSE);
        g_assert(g_node_is_ancestor(finalsuccess, tail) == FALSE);
        g_assert(g_node_is_ancestor(tail, finalnode) == FALSE);

        // Keep track of how much time we're collapsing.
        collapsedtime += path_total_elapsed(tail);

        // Cleanup all tasks on this retired tree.
        g_node_traverse(head,
                        G_PRE_ORDER,
                        G_TRAVERSE_ALL,
                        -1,
                        abort_task_helper,
                        NULL);

        // Put it in the retired tree for cleanup by cleanup_tree()
        g_node_insert(retired, -1, head);
    }

    // Return the new depth so that status messages are consistent.
    return g_node_depth(find_finalized_node(tree, true));
}

static gint print_status_message(GTimer *elapsed, gint finaldepth)
{
    GNode  *finalnode;
    task_t *finaltask;
    gdouble finalelapsed;

    if (kQuiet == true)
        return -1;

    finalnode    = find_finalized_node(tree, true);
    finaltask    = finalnode->data;

    // We count the elapsed time to the last finalized node regardless of
    // success, this makes the user time calculation more accurate.
    finalelapsed = path_total_elapsed(find_finalized_node(tree, false));

    // We may have removed nodes to prune large trees.
    finalelapsed += collapsedtime;

    // Print status messages if this is a terminal.
    if (isatty(STDOUT_FILENO)) {
            printf("treesize=%u, height=%u, unproc=%u, real=%.1fs, user=%.1fs, speedup=~%.1fs\r",
                    g_node_n_nodes(tree, G_TRAVERSE_ALL)
                        + g_node_n_nodes(retired, G_TRAVERSE_ALL),
                    g_node_max_height(tree)
                        + g_node_n_nodes(retired, G_TRAVERSE_ALL),
                    g_thread_pool_unprocessed(threadpool),
                    g_timer_elapsed(elapsed, NULL),
                    finalelapsed,
                    finalelapsed - g_timer_elapsed(elapsed, NULL));
    }

    if (g_node_depth(finalnode) > finaldepth) {
        finaldepth = g_node_depth(finalnode);
        g_message("New finalized size: %lu (depth=%u) real=%.1fs, user=%.1fs, speedup=~%.1fs",
                finaltask->size,
                g_node_depth(finalnode)
                    + g_node_n_nodes(retired, G_TRAVERSE_ALL),
                g_timer_elapsed(elapsed, NULL),
                finalelapsed,
                finalelapsed - g_timer_elapsed(elapsed, NULL));
    }

    return finaldepth;
}
