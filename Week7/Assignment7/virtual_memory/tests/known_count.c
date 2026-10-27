#include "../workloads/common.h"
#include <pthread.h>
#include <stdatomic.h>
static unsigned pairs=100000;
static atomic_uint ready;
static atomic_int go;
static void *worker(void *arg){(void)arg;atomic_fetch_add(&ready,1);while(!atomic_load(&go))wait_ms(1);for(unsigned i=0;i<pairs;i++){void *p=alloc_bytes(32);((volatile char *)p)[0]=1;free(p);}return NULL;}
int main(int argc,char **argv){int interactive=0;if(argc>1){if(strcmp(argv[1],"--checkpoints")){fputs("usage: known_count [--checkpoints]\n",stderr);return 2;}interactive=1;}
 setvbuf(stdout,NULL,_IONBF,0);setvbuf(stdin,NULL,_IONBF,0);pthread_t t[4];
 for(int i=0;i<4;i++)if(pthread_create(t+i,NULL,worker,NULL))return 1;
 while(atomic_load(&ready)<4)wait_ms(1);
 if(interactive)checkpoint("before_pairs");else phase("before_pairs");
 atomic_store(&go,1);for(int i=0;i<4;i++)pthread_join(t[i],NULL);
 if(interactive)checkpoint("after_pairs");else phase("after_pairs");
 puts("EXPECTED workload_malloc=400000 workload_free=400000; runtime overhead is separate");return 0;}
