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

#include <sys/resource.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "task.h"
#include "util.h"
#include "flags.h"

// This file is part of halfempty - a fast, parallel testcase minimization tool.
//
// Various small miscellaneous utility routines.
//

gsize g_file_size(gint fd)
{
    struct stat buf;

    g_assert_cmpint(fd, >=, 0);

    if (fstat(fd, &buf) != 0) {
        g_error("failed to stat file descriptor %d", fd);
    }

    return buf.st_size;
}

void g_message_quiet(const gchar *string)
{
    if (!kQuiet) {
        g_clearline();
        puts(string);
    }
}

void g_clearline(void)
{
    if (isatty(STDOUT_FILENO) && !kQuiet) {
        printf("\e[0K");
    }
}

static gboolean tree_depth_helper(GNode *node, gpointer data)
{
    guint *maxdepth = (guint*)data;

    if (g_node_depth(node) > *maxdepth)
        *maxdepth = g_node_depth(node);
    return false;
}

// Find the depth of the deepest node in the tree.
guint find_maximum_depth(GNode *root)
{
    guint maxdepth = 0;

    g_node_traverse(root, G_LEVEL_ORDER, G_TRAVERSE_LEAVES, -1, tree_depth_helper, &maxdepth);

    return maxdepth;
}

static gboolean draw_tree_helper(GNode *node, gpointer user)
{
    FILE *out = (FILE*)user;
    task_t *data = node->data;
    static const gchar *taskcolor[] = {
        [TASK_STATUS_FAILURE] = "red",
        [TASK_STATUS_SUCCESS] = "green",
        [TASK_STATUS_PENDING] = "orange",
        [TASK_STATUS_DISCARDED] = "grey",
    };

    if (data) {
        if (data->status == TASK_STATUS_DISCARDED && kSimplifyDotFile) {
            // Simplify the graph by ignoring discarded branches.
            return false;
        }
        fprintf(out, "\"%p\" [label=\"%lu bytes\" style=filled fillcolor=%s];\n",
                     node,
                     data->size,
                     taskcolor[data->status]);
    }

    if (node->children) {
        if (node->children->data) {
            fprintf(out, " \"%p\" -> \"%p\" [label=\"Failure\"];\n", node, node->children);
        }
        if (node->children->next && node->children->next->data) {
            fprintf(out, " \"%p\" -> \"%p\" [label=\"Success\"];\n", node, node->children->next);
        }
    }

    return false;
}

// Generate a DOT file from the specified binary tree.
gboolean generate_dot_tree(GNode *root, gchar *filename)
{
    FILE *out = fopen(filename, "w");

    if (!out) {
        g_warning("failed to open file `%s` to save dot file, %s", filename, strerror(errno));
        return false;
    }

    fprintf(out, "digraph tree { node [fontname=Arial];\n");

    if (root) {
        // OK, well, this is about the limit of how useful the graph is.
        if (g_node_n_nodes(root, G_TRAVERSE_ALL) > 100)
            kSimplifyDotFile = true;

        g_node_traverse(root, G_PRE_ORDER, G_TRAVERSE_ALL, -1, draw_tree_helper, out);
    }

    fprintf(out, "}\n");
    fclose(out);
    return true;
}

gint g_unlinked_tmp(GError **error)
{
    gint fd = -1;
    gchar *filename;

#ifdef O_TMPFILE
    fd = open("/tmp", O_TMPFILE | O_RDWR, 0600);
#endif

    // O_TMPFILE can be defined but still fail on some systems.
    if (fd == -1) {
        fd = g_file_open_tmp(NULL, &filename, error);

        if (fd != -1) {
            g_unlink(filename);
            g_free(filename);
        }
    }
    return fd;
}

// A more convenient wrapper for sendfile.
gssize g_sendfile(gint outfd, gint infd, goffset offset, gsize count)
{
    return sendfile(outfd, infd, &offset, count);
}

// A more convenient wrapper for sendfile.
gboolean g_sendfile_all(gint outfd, gint infd, goffset offset, gsize count)
{
    gsize result;
    gsize total;

    total = 0;

    while (total < count) {
        result = g_sendfile(outfd, infd, offset, count - total);

        if (result < 0)
           break;

        total += result;

        g_assert_cmpint(total, <=, count);
    }

    if (total != count)
        g_debug("failed to write requested amount of data, will return fail");

    return total == count;
}

// Empty log handler to silence messages.
void g_log_null_handler(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data)
{
    return;
}

// Automatically generated from the monitor.tpl file.
static const gchar kMonitorHtml[] = {
    #include "monitor.h"
    0,
};

static void __attribute__((destructor)) cleanup(void)
{
    if (kMonitorTmpImageFilename)
        g_unlink(kMonitorTmpImageFilename);
    if (kMonitorTmpHtmlFilename)
        g_unlink(kMonitorTmpHtmlFilename);
    g_free(kMonitorTmpImageFilename);
    g_free(kMonitorTmpHtmlFilename);
}

// Debugging utility, generate some html so you can monitor the progress in a browser.
gboolean generate_monitor_image(GNode *root)
{
    gchar *commandline;
    gchar *tmpdotfile;
    gchar *tmpimgfile;
    gchar *html;

    if (!kMonitorTmpHtmlFilename) {
        g_close(g_file_open_tmp("halfout-XXXXXX.png", &kMonitorTmpImageFilename, NULL), NULL);
        g_close(g_file_open_tmp("halfout-XXXXXX.htm", &kMonitorTmpHtmlFilename, NULL), NULL);

        html = g_strdup_printf(kMonitorHtml, kMonitorTmpImageFilename);
        g_file_set_contents(kMonitorTmpHtmlFilename, html, -1, NULL);
        g_message("Use the URL <file://%s> for monitor mode.", kMonitorTmpHtmlFilename);
        g_free(html);
    }

    // Save the dot stuff.
    g_close(g_file_open_tmp(NULL, &tmpdotfile, NULL), NULL);
    g_close(g_file_open_tmp(NULL, &tmpimgfile, NULL), NULL);

    generate_dot_tree(root, tmpdotfile);

    commandline = g_strdup_printf("dot -Gsize=10 -Tpng -o %s %s", tmpimgfile, tmpdotfile);
    g_spawn_command_line_sync(commandline, NULL, NULL, NULL, NULL);
    g_rename(tmpimgfile, kMonitorTmpImageFilename);
    g_unlink(tmpdotfile);
    g_free(commandline);
    g_free(tmpdotfile);
    g_free(tmpimgfile);
    return true;
}
