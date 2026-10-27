#include "common.h"
int initialized_global=7;
int second_global=11;
const char read_only_object[]="assignment seven";
unsigned char large_bss[8*1024*1024];
int main(void){int local=1;void *p=alloc_bytes(1024);touch(p,1024);
 printf("initialized_global=%p second_global=%p readonly=%p\n",(void *)&initialized_global,(void *)&second_global,(void *)read_only_object);
 printf("bss_first=%p bss_last=%p local=%p dynamic=%p\n",(void *)large_bss,(void *)(large_bss+sizeof large_bss-1),(void *)&local,p);
 checkpoint("layout");free(p);return local-1;}
