#include "common.h"
#include <fcntl.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 5 || (argc == 5 && strcmp(argv[4], "random"))) {
        fprintf(stderr, "usage: file_reader PATH SECONDS [MIB_PER_SECOND [random]]\n");
        return 2;
    }

    long seconds = number(argv[2], 1, 30);
    long mib_per_second = argc >= 4 ? number(argv[3], 0, 1000) : 0;
    int fd = open(argv[1], O_RDONLY);

    if (fd < 0) {
        fail("open reader");
    }

    /* KEY: bound speculative readahead in the global reclaim comparison. */
    if (argc == 5) {
        int error = posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);

        if (error != 0) {
            errno = error;
            fail("fadvise random");
        }

    }

    char *buffer = malloc(1024 * 1024);

    if (!buffer) {
        fail("malloc read buffer");
    }

    /* KEY: prepare cache in this cgroup; exclude warm-up reads from timing. */
    for (;;) {
        ssize_t count = read(fd, buffer, 1024 * 1024);

        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (count < 0) {
            fail("warm-up read");
        }

        if (count == 0) {
            break;
        }

    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        fail("warm-up rewind");
    }

    ready();
    double start = now();
    unsigned long long bytes = 0;

    /* KEY: real read calls reuse file cache or fault data back from storage. */
    while (now() - start < seconds) {
        ssize_t count = read(fd, buffer, 1024 * 1024);

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }

            fail("read");
        }

        if (count == 0) {
            if (lseek(fd, 0, SEEK_SET) < 0) {
                fail("rewind");
            }

            continue;
        }

        bytes += (unsigned long long)count;

        /* KEY: pacing leaves time for background reclaim between file reads. */
        if (mib_per_second > 0) {
            double delay = start + (double)bytes / (mib_per_second * 1048576.0) - now();

            if (delay > 0) {
                struct timespec pause = {
                    .tv_sec = (time_t)delay,
                    .tv_nsec = (long)((delay - (time_t)delay) * 1e9)
                };

                while (nanosleep(&pause, &pause) != 0) {
                    if (errno != EINTR) {
                        fail("reader pacing");
                    }

                }

            }

        }
    }

    printf("bytes=%llu seconds=%.6f\n", bytes, now() - start);
    free(buffer);
    close(fd);
    return 0;
}
