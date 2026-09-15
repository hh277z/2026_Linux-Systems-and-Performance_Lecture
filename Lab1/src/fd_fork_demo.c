#include "lab.h"
#include <fcntl.h>
#include <sys/wait.h>

/* Pipes only coordinate the demonstration. Students need not implement IPC. */
typedef struct {
    int valid, value, count;
    long long offset;
    char data[4], info[512];
} Snapshot;

static char temp_path[] = "./.fd-lab-XXXXXX";
static pid_t owner;

static void cleanup(void) {
    if (getpid() == owner) {
        unlink(temp_path);
    }
}

static void transfer(int fd, void *buffer, size_t length, int writing) {
    char *p = buffer;
    while (length) {
        ssize_t n = writing ? write(fd, p, length) : read(fd, p, length);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            if (n == 0) {
                errno = EPIPE;
            }
            die("coordination pipe");
        }
        p += n;
        length -= (size_t)n;
    }
}

static Snapshot snapshot(int fd, int value) {
    Snapshot s = {0};
    s.value = value;
    s.valid = fcntl(fd, F_GETFD) >= 0;
    s.offset = -1;
    if (!s.valid) {
        snprintf(s.info, sizeof s.info, "FD closed (no /proc open attempted)\n");
        return s;
    }
    off_t offset = lseek(fd, 0, SEEK_CUR);
    if (offset < 0) {
        die("lseek");
    }
    s.offset = (long long)offset;
    char path[128];
    snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(path, "r");
    if (f) {
        size_t n = fread(s.info, 1, sizeof s.info - 1, f);
        s.info[n] = '\0';
        fclose(f);
    } else {
        snprintf(s.info, sizeof s.info,
                 "fdinfo unavailable; lseek remains observable\n");
    }
    return s;
}

static void show(const char *who, Snapshot s) {
    printf("%s fd_open=%d pos=%lld private_value=%d\n", who, s.valid, s.offset,
           s.value);
    printf("%s /proc/self/fdinfo contents:\n%s", who, s.info);
}

static Snapshot ask(int out, int in, char command) {
    Snapshot s;
    transfer(out, &command, 1, 1);
    transfer(in, &s, sizeof s, 0);
    return s;
}

static ssize_t read_data(int fd, char *buf, size_t size, int positioned) {
    size_t got = 0;
    while (got < size) {
        ssize_t n = positioned ? pread(fd, buf + got, size - got, (off_t)got)
                               : read(fd, buf + got, size - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            die("read/pread");
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    return (ssize_t)got;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 ||
        (strcmp(argv[1], "shared") && strcmp(argv[1], "reopen") &&
         strcmp(argv[1], "pread")) ||
        (argc == 3 && strcmp(argv[2], "--auto"))) {
        fprintf(stderr, "Usage: %s shared|reopen|pread [--auto]\n", argv[0]);
        return 2;
    }

    int automatic = argc == 3;
    owner = getpid();
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        die("mkstemp in current directory");
    }
    if (atexit(cleanup)) {
        unlink(temp_path);
        fputs("atexit failed\n", stderr);
        return 1;
    }

    char contents[] = "ABCDEFGHIJ";
    transfer(fd, contents, 10, 1);
    if (lseek(fd, 0, SEEK_SET) < 0) {
        die("lseek");
    }

    int value = 7;
    printf("mode=%s file=%s fd=%d parent_pid=%ld\n", argv[1], temp_path, fd,
           (long)getpid());
    show("BEFORE_FORK_PARENT", snapshot(fd, value));
    checkpoint(automatic,
               "Predict which file position will change when the child reads.");

    int commands[2], replies[2];
    if (pipe(commands) || pipe(replies)) {
        die("pipe");
    }

    fflush(NULL); /* Do not duplicate a buffered pre-fork message. */
    pid_t child = fork();
    if (child < 0) {
        die("fork");
    }
    if (child == 0) {
        close(commands[1]);
        close(replies[0]);
        if (!strcmp(argv[1], "reopen")) {
            if (close(fd)) {
                die("close inherited fd");
            }
            fd = open(temp_path, O_RDONLY);
            if (fd < 0) {
                die("reopen");
            }
        }

        for (;;) {
            char command;
            transfer(commands[0], &command, 1, 0);
            if (command == 'Q') {
                break;
            }
            char data[4] = {0};
            int count = 0;
            if (command == 'R') {
                ++value;
                count = (int)read_data(fd, data, 3, !strcmp(argv[1], "pread"));
            } else if (command == 'C') {
                if (close(fd)) {
                    die("child close");
                }
            }
            Snapshot s = snapshot(fd, value);
            s.count = count;
            memcpy(s.data, data, sizeof data);
            transfer(replies[1], &s, sizeof s, 1);
        }
        close(commands[0]);
        close(replies[1]);
        _exit(0);
    }

    close(commands[0]);
    close(replies[1]);
    printf("child_pid=%ld\n", (long)child);
    Snapshot cs = ask(commands[1], replies[0], 'S');
    show("AFTER_FORK_PARENT", snapshot(fd, value));
    show("AFTER_FORK_CHILD", cs);
    checkpoint(automatic,
               "Next: child reads 3 bytes and increments its private variable.");

    cs = ask(commands[1], replies[0], 'R');
    printf("CHILD_READ=%s count=%d\n", cs.data, cs.count);
    Snapshot ps = snapshot(fd, value);
    show("AFTER_READ_PARENT", ps);
    show("AFTER_READ_CHILD", cs);
    printf("AFTER_CHILD_READ parent_pos=%lld child_pos=%lld parent_value=%d "
           "child_value=%d\n",
           ps.offset, cs.offset, ps.value, cs.value);
    checkpoint(automatic, "Next: child closes its FD; parent then reads 2 bytes.");

    cs = ask(commands[1], replies[0], 'C');
    show("AFTER_CLOSE_CHILD", cs);
    char data[3] = {0};
    if (read_data(fd, data, 2, 0) != 2) {
        fputs("Unexpected short fixture\n", stderr);
        return 1;
    }
    printf("PARENT_READ=%s\n", data);
    show("FINAL_PARENT", snapshot(fd, value));

    char quit = 'Q';
    transfer(commands[1], &quit, 1, 1);
    close(commands[1]);
    close(replies[0]);
    close(fd);

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            die("waitpid");
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fputs("Child failed\n", stderr);
        return 1;
    }

    puts("Child reaped; temporary fixture removed on normal exit.");
    return 0;
}
