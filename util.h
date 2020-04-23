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

#ifndef __UTIL_H
#define __UTIL_H

// This file is part of halfempty - a fast, parallel testcase minimization tool.

gsize g_file_size(gint fd);
guint find_maximum_depth(GNode *root);
gboolean generate_dot_tree(GNode *root, gchar *filename);
gint g_unlinked_tmp(GError **error);
gssize g_sendfile(gint outfd, gint infd, goffset offset, gsize count);
gboolean g_sendfile_all(gint outfd, gint infd, goffset offset, gsize count);
gssize g_splice(gint infd, goffset offset, gint outfd, gsize count);
void g_log_null_handler(const gchar *log_domain,
                        GLogLevelFlags log_level,
                        const gchar *message,
                        gpointer user_data);
void g_message_quiet(const gchar *string);
void g_clearline(void);
gboolean generate_monitor_image(GNode *root);

#ifdef SPLICE_GENERIC
ssize_t splice(int fd_in,
               off_t *off_in,
               int fd_out,
               off_t *off_out,
               size_t len,
               unsigned int flags);
#endif

#ifdef SENDFILE_GENERIC
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
#endif

#else
# warning util.h included twice
#endif
