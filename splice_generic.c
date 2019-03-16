#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <glib.h>

#ifdef SENDFILE_GENERIC
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
#endif

ssize_t splice(int fd_in,
               off_t *off_in,
               int fd_out,
               off_t *off_out,
               size_t len,
               unsigned int flags)
{
    return sendfile(fd_out, fd_in, off_in, len);
}
