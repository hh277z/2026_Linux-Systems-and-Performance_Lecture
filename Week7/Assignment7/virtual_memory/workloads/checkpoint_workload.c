#include "common.h"
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stdin,NULL,_IONBF,0);
    void *small[1000];checkpoint("baseline");
    for(int i=0;i<1000;i++){small[i]=alloc_bytes(1024);touch(small[i],1024);}
    checkpoint("A");void *large=alloc_bytes(16*1024*1024);touch(large,16*1024*1024);
    checkpoint("B");free(large);checkpoint("C");
    for(int i=0;i<1000;i++)free(small[i]);
    checkpoint("D");return 0;
}
