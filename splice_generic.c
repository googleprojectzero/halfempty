#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <glib.h>

ssize_t splice(int fd_in, off_t *off_in, int fd_out, off_t *off_out, size_t len, unsigned int flags)
{
    return sendfile(fd_out, fd_in, off_in, len);
}
