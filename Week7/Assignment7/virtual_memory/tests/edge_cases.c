#include "../workloads/common.h"
#include <stdint.h>
int main(void){
 errno=EDOM;free(NULL);if(errno!=EDOM)return 1;
 void *z=malloc(0);free(z);z=calloc(0,19);free(z);z=calloc(19,0);free(z);
 void *p=realloc(NULL,128);if(!p)return 1;memset(p,0x5a,128);
 volatile size_t huge=SIZE_MAX;errno=0;void *q=realloc(p,huge);
 if(q || errno!=ENOMEM || ((unsigned char *)p)[0]!=0x5a){fputs("failed realloc contract\n",stderr);return 1;}free(p);
 errno=0;q=calloc(huge,16);if(q||errno!=ENOMEM){fputs("calloc overflow contract\n",stderr);return 1;}
 p=alloc_bytes(64);q=realloc(p,0);if(q){fputs("target glibc realloc(p,0) differs\n",stderr);free(q);return 1;}
 errno=ERANGE;free(NULL);if(errno!=ERANGE)return 1;
 puts("EDGE_OK: semantics checked; inspect statistics separately");return 0;}
