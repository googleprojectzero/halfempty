#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <glib.h>

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
     char buf[BUFSIZ];
     size_t total = 0;

     if (lseek(in_fd, *offset, SEEK_SET) < 0)
         return -1;

     while (total < count) {
         ssize_t r = 0;
         ssize_t w = 0;
         size_t written = 0;

         size_t next = sizeof(buf);
         if (next > count - total)
             next = count - total;

         r = read(in_fd, buf, next);
         if (r < 0)
             return r;

         while (written < (size_t)r) {
             w = write(out_fd, buf, (size_t)r);
             if (w < 0)
                 return w;

             written += (size_t)w;
             g_assert_cmpint(w, <=, r);
             r -= w;
         }

         total += written;
     }

     return total;
}
