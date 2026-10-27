#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "mtrace_student.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Provided infrastructure; statistics belong in mtrace_student.c.
 * Fixed, max_align_t-aligned bootstrap arena; storage lasts for process lifetime.
 * Recursive allocations during dlsym use the arena. Other threads wait until
 * all four original pointers are published. Bootstrap pointers are range-tested
 * before their headers are read. Arena exhaustion returns NULL/ENOMEM.
 */
enum { BOOT_CAP = 1024 * 1024 };
static alignas(max_align_t) unsigned char arena[BOOT_CAP];
static _Atomic size_t used;
static _Atomic int state;
static _Thread_local int internal;
static _Thread_local int resolving;
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t,size_t);
static void *(*real_realloc)(void *,size_t);
static void (*real_free)(void *);
static pthread_mutex_t stats = PTHREAD_MUTEX_INITIALIZER;
static int out = STDERR_FILENO;
static int notification[2] = {-1,-1};
static pthread_t reporter;
static int reporter_started;
static struct sigaction previous;
static volatile sig_atomic_t signal_fd = -1;

void mt_text(int fd,const char *s) {
    size_t n=0; while(s[n]) n++;
    while(n) { ssize_t k=write(fd,s,n); if(k<0 && errno==EINTR) continue;
        if(k<=0) break;
        s+=k; n-=(size_t)k; }
}
void mt_number(int fd,uint64_t v) {
    char b[32]; size_t n=sizeof b; b[--n]=0;
    do { b[--n]=(char)('0'+v%10); v/=10; } while(v);
    mt_text(fd,b+n);
}
static int is_boot(const void *p) {
    uintptr_t a=(uintptr_t)p, b=(uintptr_t)arena;
    return a>=b+alignof(max_align_t) && a<b+BOOT_CAP;
}
static void *boot(size_t n) {
    size_t a=alignof(max_align_t), h=a;
    if(n>BOOT_CAP-h-a) {errno=ENOMEM;return NULL;}
    size_t span=(h+(n?n:1)+a-1)/a*a;
    size_t offset=atomic_fetch_add_explicit(&used,span,memory_order_relaxed);
    if(offset>BOOT_CAP || span>BOOT_CAP-offset) {errno=ENOMEM;return NULL;}
    memcpy(arena+offset,&n,sizeof n); return arena+offset+h;
}
static void *boot_resize(void *p,size_t n) {
    if(!n) return NULL;
    size_t old; memcpy(&old,(unsigned char *)p-alignof(max_align_t),sizeof old);
    void *q=boot(n); if(q) memcpy(q,p,old<n?old:n);return q;
}
static void resolve(void) {
    if(atomic_load_explicit(&state,memory_order_acquire)==2 || resolving) return;
    int expected=0;
    if(atomic_compare_exchange_strong(&state,&expected,1)) {
        resolving=1;
        *(void **)(&real_malloc)=dlsym(RTLD_NEXT,"malloc");
        *(void **)(&real_calloc)=dlsym(RTLD_NEXT,"calloc");
        *(void **)(&real_realloc)=dlsym(RTLD_NEXT,"realloc");
        *(void **)(&real_free)=dlsym(RTLD_NEXT,"free");
        if(!real_malloc||!real_calloc||!real_realloc||!real_free) {
            mt_text(2,"mtrace: cannot resolve glibc allocator\n");_exit(127);
        }
        resolving=0; atomic_store_explicit(&state,2,memory_order_release);
    } else while(atomic_load_explicit(&state,memory_order_acquire)!=2) sched_yield();
}
void *malloc(size_t n) {
    resolve(); if(resolving) return boot(n);
    if(internal) return real_malloc(n);
    internal++; pthread_mutex_lock(&stats);
    void *p=real_malloc(n);int saved=errno;mt_event(MT_MALLOC,0,p,n,0);
    pthread_mutex_unlock(&stats);internal--;errno=saved;return p;
}
void *calloc(size_t n,size_t s) {
    int overflow=s && n>SIZE_MAX/s;
    resolve();
    if(resolving) {if(overflow){errno=ENOMEM;return NULL;}void *p=boot(n*s);if(p)memset(p,0,n*s);return p;}
    if(internal) return real_calloc(n,s);
    internal++;pthread_mutex_lock(&stats);
    void *p=real_calloc(n,s);int saved=errno;
    mt_event(MT_CALLOC,0,p,overflow?0:n*s,overflow);
    pthread_mutex_unlock(&stats);internal--;errno=saved;return p;
}
void free(void *p) {
    int saved=errno;
    if(is_boot(p)){errno=saved;return;}
    resolve(); if(resolving){errno=saved;return;}
    if(internal){real_free(p);errno=saved;return;}
    uintptr_t old=(uintptr_t)p;internal++;pthread_mutex_lock(&stats);
    real_free(p); mt_event(MT_FREE,old,NULL,0,0);
    pthread_mutex_unlock(&stats);internal--;errno=saved;
}
void *realloc(void *p,size_t n) {
    if(is_boot(p)) {
        /* Remain in bootstrap storage; never mix this ownership with glibc. */
        return boot_resize(p,n);
    }
    resolve();if(resolving) return p?boot_resize(p,n):boot(n);
    if(internal) return real_realloc(p,n);
    uintptr_t old=(uintptr_t)p;internal++;pthread_mutex_lock(&stats);
    void *q=real_realloc(p,n);int saved=errno;mt_event(MT_REALLOC,old,q,n,0);
    pthread_mutex_unlock(&stats);internal--;errno=saved;return q;
}
static void notify(int sig) {
    (void)sig;int saved=errno;unsigned char byte=1;
    if(signal_fd>=0) {ssize_t ignored=write(signal_fd,&byte,1);(void)ignored;}
    errno=saved;
}
static void *report_loop(void *unused) {
    (void)unused;internal=1;unsigned char bytes[64];
    for(;;) {
        ssize_t n=read(notification[0],bytes,sizeof bytes);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) break;
        for(ssize_t i=0;i<n;i++) if(bytes[i]==0) return NULL;
        pthread_mutex_lock(&stats);mt_report(out,"SIGUSR1");pthread_mutex_unlock(&stats);
    }return NULL;
}
__attribute__((constructor)) static void start(void) {
    internal++;resolve();const char *name=getenv("MTRACE_OUT");
    if(name && *name) {int fd=open(name,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
        if(fd<0)mt_text(2,"mtrace: cannot open MTRACE_OUT; using stderr\n");else out=fd;}
    if(pipe2(notification,O_CLOEXEC)==0) {
        int flags=fcntl(notification[1],F_GETFL);fcntl(notification[1],F_SETFL,flags|O_NONBLOCK);
        if(pthread_create(&reporter,NULL,report_loop,NULL)==0) {
            reporter_started=1;signal_fd=notification[1];
            struct sigaction sa={0};sa.sa_handler=notify;sa.sa_flags=SA_RESTART;sigemptyset(&sa.sa_mask);
            if(sigaction(SIGUSR1,&sa,&previous)!=0) {_exit(127);}
        }else {_exit(127);}
    }else {_exit(127);}
    internal--;
}
__attribute__((destructor)) static void stop(void) {
    internal++;signal_fd=-1;
    if(reporter_started) {
        sigaction(SIGUSR1,&previous,NULL);
        unsigned char zero=0;
        /* A full pipe is drained by the reporter; never hold stats while joining. */
        while(write(notification[1],&zero,1)<0 && (errno==EAGAIN||errno==EINTR))sched_yield();
        pthread_join(reporter,NULL);
        close(notification[0]);close(notification[1]);
    }
    pthread_mutex_lock(&stats);mt_report(out,"exit");pthread_mutex_unlock(&stats);
    if(out!=2)close(out);
    internal--;
}
