#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

static pid_t children[64];
static int count, gate=-1;
static void *region=MAP_FAILED;
static size_t length;
static volatile sig_atomic_t interrupted;

static void on_signal(int sig) {
    (void)sig;
    interrupted=1;
}

static void cleanup(void) {
    if(gate>=0) {
        close(gate);
        gate=-1;
    }

    for(int i=0;i<count;i++) {
        while(waitpid(children[i],NULL,0)<0 && errno==EINTR) {
        }
    }

    count=0;

    if(region!=MAP_FAILED) {
        munmap(region,length);
        region=MAP_FAILED;
    }
}

static void fail(const char *s) {
    perror(s);
    exit(1);
}

static void stage(const char *name,int automatic) {
    printf("\nSTAGE %s PID=%ld children=%d\n",name,(long)getpid(),count);
    FILE *f=fopen("/proc/self/maps","r");
    char line[2048];
    int maps=0;

    if(!f)
        fail("maps");

    while(fgets(line,sizeof line,f)) {
        if(strchr(line,'\n'))
            maps++;
    }

    fclose(f);
    printf("SELF_MAPS_LINES=%d\n",maps);

    f=fopen("/proc/slabinfo","r");

    if(f) {
        puts("name active_objs num_objs objsize objperslab pagesperslab ...");

        while(fgets(line,sizeof line,f))
            if(!strncmp(line,"task_struct ",12) || !strncmp(line,"vm_area_struct ",15))
                fputs(line,stdout);

        fclose(f);
    } else
        puts("Read slabinfo in terminal B with sudo (workload needs no sudo).");

    if(!automatic) {
        puts("Observe in terminal B, then press Enter.");

        if(interrupted || getchar()==EOF)
            exit(interrupted?130:2);
    }

    if(interrupted)
        exit(130);
}

int main(int argc,char **argv) {
    int automatic=argc==3 && !strcmp(argv[2],"--auto");

    if(argc<2 || argc>3 || (argc==3 && !automatic) ||
       (strcmp(argv[1],"task") && strcmp(argv[1],"vma"))) {
        fprintf(stderr,"usage: %s task|vma [--auto]\n",argv[0]);
        return 2;
    }

    setbuf(stdout,NULL);
    atexit(cleanup);
    struct sigaction sa={0};
    sa.sa_handler=on_signal;
    sigemptyset(&sa.sa_mask);

    if(sigaction(SIGINT,&sa,NULL) || sigaction(SIGTERM,&sa,NULL))
        fail("sigaction");

    stage("BASELINE",automatic);

    if(!strcmp(argv[1],"task")) {
        int fd[2];

        if(pipe(fd))
            fail("pipe");

        gate=fd[1];

        for(int i=0;i<64;i++) {
            if(interrupted) {
                close(fd[0]);
                exit(130);
            }

            /* KEY: each child needs task_struct and its own VMA metadata. */
            pid_t pid=fork();

            if(pid<0) {
                close(fd[0]);
                fail("fork");
            }

            if(!pid) {
                close(fd[1]);
                signal(SIGINT,SIG_DFL);
                signal(SIGTERM,SIG_DFL);
                char c;

                /* KEY: keep children alive until the parent closes the pipe. */
                while(read(fd[0],&c,1)<0 && errno==EINTR) {
                }

                close(fd[0]);
                _exit(0);
            }

            children[count++]=pid;
        }

        close(fd[0]);

        stage("64_CHILDREN_ALIVE",automatic);
        cleanup();
        stage("CHILDREN_REAPED",automatic);
    } else {
        long page=sysconf(_SC_PAGESIZE);

        if(page<=0)
            fail("pagesize");

        length=2048UL*(size_t)page;

        /* KEY: no payload touch; this experiment grows VMA metadata. */
        region=mmap(NULL,length,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);

        if(region==MAP_FAILED)
            fail("mmap");

        printf("RANGE=%p..%p page=%ld bytes; NO payload touch\n",region,(char *)region+length,page);

        stage("ONE_RESERVED_RANGE",automatic);

        for(int i=0;i<1024;i++) {
            if(interrupted)
                exit(130);

            /* KEY: alternating RW/NONE pages cannot merge into one VMA. */
            if(mprotect((char *)region+2UL*(size_t)i*(size_t)page,
                        (size_t)page,PROT_READ|PROT_WRITE))
                fail("mprotect");
        }

        stage("ALTERNATING_RW_NONE",automatic);
        cleanup();
        stage("RANGE_UNMAPPED",automatic);
    }

    return 0;
}
