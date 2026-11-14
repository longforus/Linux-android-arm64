#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static volatile sig_atomic_t running = 1;
static volatile uint64_t ticks;
static void stop(int sig) { (void)sig; running = 0; }
__attribute__((noinline, aligned(4096))) static uint64_t hot(uint64_t value) {
    __asm__ volatile("nop\n\tnop" ::: "memory"); ticks++; return value + 1;
}
int main(void) {
    signal(SIGTERM, stop); signal(SIGINT, stop);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *p = mmap(NULL, page * 5, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 1;
    memset(p, 0x55, page * 5);
    float near=1.24f, outside=1.26f, exact=1.2f;
    int32_t value=0x13579bdf;
    memcpy(p+1, &near, 4); memcpy(p+page-2, &outside, 4); memcpy(p+3*page+8, &exact, 4);
    memcpy(p+32, &value, 4); memcpy(p+3*page+17, &value, 4);
    for (int i=0;i<32;i++) p[128+i]=(unsigned char)(0x80+i);
    if (mprotect(p+2*page, page, PROT_NONE)) return 2;
    printf("{\"pid\":%d,\"page\":%zu,\"base\":\"%p\",\"payload\":\"%p\",\"unaligned_int\":\"%p\",\"near\":\"%p\",\"outside\":\"%p\",\"exact\":\"%p\",\"partial\":\"%p\",\"marker\":\"%p\",\"hot\":\"%p\"}\n", getpid(),page,p,p+32,p+3*page+17,p+1,p+page-2,p+3*page+8,p+2*page-16,p+128,(void*)hot);
    fflush(stdout);
    while(running) {
        int32_t observed; memcpy(&observed,p+32,4); (void)hot((uint64_t)observed);
        if(ticks%50==0) { printf("ticks=%" PRIu64 " value=%d\n",ticks,observed);fflush(stdout); }
        usleep(20000);
    }
    munmap(p,page*5);return 0;
}
