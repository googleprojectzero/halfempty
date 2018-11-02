#include <stdint.h>

ssize_t splice(int fd_in, off_t *off_in, int fd_out, off_t *off_out, size_t len, unsigned int flags)
{
    return sendfile(fd_out, in_fd, *offset, len)
}
