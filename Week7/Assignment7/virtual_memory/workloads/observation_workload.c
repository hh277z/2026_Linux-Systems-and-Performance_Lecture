#include "common.h"
#include <pthread.h>
#include <stdatomic.h>
static atomic_int stop,ready;
static void *worker(void *p){(void)p;atomic_fetch_add(&ready,1);while(!atomic_load(&stop))wait_ms(10);return NULL;}
__attribute__((noinline)) static void descend(unsigned n){volatile unsigned char frame[4096];for(size_t i=0;i<sizeof frame;i++)frame[i]=(unsigned char)n;if(n)descend(n-1);else checkpoint("deepest");if(frame[0]==255)fputs("",stderr);}
int main(int argc,char **argv){
 if(argc<2){fprintf(stderr,"usage: %s idle [seconds] | threads | stack depth | growth [ticks]\n",argv[0]);return 2;}
 if(!strcmp(argv[1],"idle")){unsigned s=argc==3?number(argv[2],1,120):30;phase("idle");sleep(s);}
 else if(!strcmp(argv[1],"threads") && argc==2){pthread_t t[4];for(int i=0;i<4;i++)if(pthread_create(t+i,NULL,worker,NULL))return 1;while(atomic_load(&ready)<4)wait_ms(1);checkpoint("four_threads");atomic_store(&stop,1);for(int i=0;i<4;i++)pthread_join(t[i],NULL);}
 else if(!strcmp(argv[1],"stack") && argc==3)descend(number(argv[2],1,128));
 else if(!strcmp(argv[1],"growth")){unsigned ticks=argc==3?number(argv[2],1,120):12;void *p[120];phase("baseline");for(unsigned i=0;i<ticks;i++){p[i]=alloc_bytes(1024*1024);touch(p[i],1024*1024);phase("growing");wait_ms(1000);}for(unsigned i=0;i<ticks;i++)free(p[i]);}
 else {fputs("invalid mode or arguments\n",stderr);return 2;}return 0;
}
