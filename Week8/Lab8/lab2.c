#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>

static long ps;
static volatile unsigned long checksum;

static void die(const char *s) {
    perror(s);
    exit(1);
}

static void stage(const char *s, int automatic,
                  const char *next_action, const char *next_code) {
    printf("STAGE %s PID=%ld\n",s,(long)getpid());
    FILE *f=fopen("/proc/self/status","r");
    char line[256];

    if (!f)
        die("status");

    while (fgets(line,sizeof line,f))
        if (!strncmp(line,"VmSize:",7))
            fputs(line,stdout);

    fclose(f);

    f=fopen("/proc/self/smaps_rollup","r");

    if (!f)
        die("smaps_rollup");

    while (fgets(line,sizeof line,f))
        if (!strncmp(line,"Rss:",4))
            fputs(line,stdout);

    fclose(f);

    printf("\n\n============================================================\n");
    printf("NEXT / Enter 이후: %s\n", next_action);
    puts("============================================================\n");
    puts(next_code);
    puts("\n============================================================\n");

    if (!automatic) {
        puts("Enter to continue (record this stage first)");

        if (getchar()==EOF)
            exit(2);
    }
}

static void touch(volatile unsigned char *p,size_t n,int write_access,const char *name) {
    struct rusage a,b;
    struct timespec t,u;
    clock_gettime(CLOCK_MONOTONIC,&t);
    getrusage(RUSAGE_SELF,&a);

    unsigned long sum=0;

    /* KEY: first touch faults; the same loop later reuses those pages. */
    for (size_t i=0;i<n;i+=(size_t)ps) {
        if(write_access)
            p[i]=1;
        else
            sum+=p[i];
    }

    getrusage(RUSAGE_SELF,&b);
    clock_gettime(CLOCK_MONOTONIC,&u);
    checksum=sum;
    printf("%s elapsed_ms=%.3f minor=%ld major=%ld checksum=%lu\n",name,
           (u.tv_sec-t.tv_sec)*1000.0+(u.tv_nsec-t.tv_nsec)/1e6,
           b.ru_minflt-a.ru_minflt,b.ru_majflt-a.ru_majflt,sum);
}

int main(int argc,char **argv) {
    setbuf(stdout,NULL);
    ps=sysconf(_SC_PAGESIZE);

    if(ps<=0)
        die("pagesize");

    int automatic=argc>1 && !strcmp(argv[argc-1],"--auto");
    int count=argc-automatic;

    if (count==2 && !strcmp(argv[1],"anon")) {
        size_t n=32UL*1024*1024;
        puts("\n=== Lab 2-A: anonymous mmap / first touch / rewrite / munmap ===\n");
        stage("baseline",automatic,
              "mmap: reserve 32 MiB of virtual address space (no payload access)",
              "p = mmap(NULL, n, PROT_READ | PROT_WRITE,\n"
              "         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);\n"
              "madvise((void *)p, n, MADV_NOHUGEPAGE);");

        /* KEY: mmap reserves virtual space; payload pages arrive on first touch. */
        volatile unsigned char *p=mmap(NULL,n,PROT_READ|PROT_WRITE,
                                       MAP_PRIVATE|MAP_ANONYMOUS,-1,0);

        if(p==MAP_FAILED)
            die("mmap");

        if(madvise((void *)p,n,MADV_NOHUGEPAGE))
            die("MADV_NOHUGEPAGE");

        stage("mapped-not-touched",automatic,
              "first touch: write 1 byte per page; allocate physical pages on demand",
              "touch(p, n, 1, \"first-write\");\n\n"
              "// Core loop in touch(): write 1 byte per page\n"
              "for (size_t i = 0; i < n; i += (size_t)ps)\n"
              "    p[i] = 1;");

        touch(p,n,1,"first-write");
        stage("written",automatic,
              "rewrite: write to the same pages; compare faults and elapsed time",
              "touch(p, n, 1, \"rewrite\");\n\n"
              "// Repeat the same write loop on the same mapping\n"
              "for (size_t i = 0; i < n; i += (size_t)ps)\n"
              "    p[i] = 1;");

        touch(p,n,1,"rewrite");
        stage("rewritten",automatic,
              "munmap: release the entire 32 MiB mapping",
              "munmap((void *)p, n);");

        if(munmap((void *)p,n))
            die("munmap");

        stage("unmapped",automatic,
              "finish: exit after the final observation",
              "return 0;");

        return 0;
    }

    if(count!=5 || strcmp(argv[1],"file") ||
       (strcmp(argv[3],"cold") && strcmp(argv[3],"warm")) ||
       (strcmp(argv[4],"random") && strcmp(argv[4],"normal"))) {
        fprintf(stderr,"usage: %s anon [--auto]\n       %s file PATH cold|warm random|normal [--auto]\n",argv[0],argv[0]);
        return 2;
    }

    int fd=open(argv[2],O_RDONLY);

    if(fd<0)
        die("open");

    struct stat st;

    if(fstat(fd,&st))
        die("fstat");

    if(!S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>256L*1024*1024) {
        fprintf(stderr,"Use a regular, fully written file of 1..256 MiB\n");
        return 2;
    }

    size_t n=(size_t)st.st_size;
    puts("\n=== Lab 2-B: page cache setup / first read / reread ===\n");

    if(!strcmp(argv[3],"cold")) {
        /* KEY: request cache eviction; this does not guarantee a cold cache. */
        int e=posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED);

        if(e) {
            errno=e;
            die("fadvise");
        }

        puts("cold requested via per-file DONTNEED (not guaranteed)");
    } else {
        unsigned char buf[65536];
        ssize_t r;

        /* KEY: warm the page cache before mmap; exclude this read from timing. */
        while((r=read(fd,buf,sizeof buf))>0)
            checksum+=buf[0];

        if(r<0)
            die("read warmup");

        puts("warmup read completed outside measured interval");
    }

    volatile unsigned char *p=mmap(NULL,n,PROT_READ,MAP_PRIVATE,fd,0);

    if(p==MAP_FAILED)
        die("mmap");

    /* KEY: RANDOM limits mmap readahead; NORMAL uses the default policy. */
    if(madvise((void *)p,n,!strcmp(argv[4],"random")?MADV_RANDOM:MADV_NORMAL))
        die("madvise");

    stage("file-mapped",automatic,"first read: read 1 byte per page from the file mapping",
          "touch(p, n, 0, \"file-first-read\");");

    touch(p,n,0,"file-first-read");
    stage("file-resident",automatic,"reread: read the same file mapping again",
          "touch(p, n, 0, \"file-reread\");");

    touch(p,n,0,"file-reread");
    stage("file-reread-done",automatic,"cleanup: munmap, close, and exit",
          "munmap((void *)p, n);\nclose(fd);\nreturn 0;");

    if(munmap((void *)p,n))
        die("munmap");

    close(fd);
    return 0;
}
