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

#ifndef __TREE_H
#define __TREE_H

// This file is part of halfempty - a fast, parallel testcase minimization tool.

#define g_node_success(n) g_node_nth_child(n, TRUE)
#define g_node_failure(n) g_node_nth_child(n, FALSE)

gboolean build_bisection_tree(gint fd,
                              strategy_cb_t callback,
                              gint *outfd,
                              gulong flags);
void cleanup_orphaned_tasks(task_t *task);
void abort_pending_tasks(GNode *root);
void process_execute_jobs(GNode *node);

typedef struct {
    const gchar *name;
    const gchar *description;
    const GOptionEntry *options;
    strategy_cb_t callback;
} strategy_t;

#define MAX_STRATEGIES 128

extern gint kNumStrategies;
extern strategy_t kStrategyList[MAX_STRATEGIES];

#define REGISTER_STRATEGY(_name, _desc, _options, _callback)            \
    static void __attribute__((constructor)) __init__ ## _name (void)   \
    {                                                                   \
        kStrategyList[kNumStrategies].name          = # _name;          \
        kStrategyList[kNumStrategies].options       = _options;         \
        kStrategyList[kNumStrategies].description   = _desc;            \
        kStrategyList[kNumStrategies].callback      = _callback;        \
        kNumStrategies++;                                               \
        g_assert_cmpint(kNumStrategies, <, MAX_STRATEGIES);             \
    }

#define BISECT_FLAG_NOFLAGS    (0)
#define BISECT_FLAG_CLOSEINPUT (1 << 0)

#else
# warning tree.h included twice
#endif
