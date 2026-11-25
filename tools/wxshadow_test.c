#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/wait.h>

static void *code_page;

static void on_fork(int sig)
{
    pid_t pid;
    (void)sig;
    pid = fork();
    if (pid == 0) {
        _exit(0); /* child exits immediately (tests inherited shadow teardown) */
    }
    waitpid(pid, NULL, 0);
    write(2, "[test] fork+reap done\n", 22);
}

static void on_mprotect(int sig)
{
    (void)sig;
    if (mprotect(code_page, 4096, PROT_NONE) != 0) {
        perror("mprotect PROT_NONE");
        return;
    }
    write(2, "[test] mprotect PROT_NONE done\n", 30);
    if (mprotect(code_page, 4096, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect r-x");
        return;
    }
    write(2, "[test] mprotect r-x done\n", 25);
    /* touch it to force a fault through the (possibly orphaned) mapping */
    volatile unsigned int *p = (unsigned int *)code_page;
    write(2, "[test] readback after mprotect\n", 31);
    (void)*p;
}

int main(void)
{
    code_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code_page == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* mov x0, #42 ; ret  ->  40 05 80 d2  c0 03 5f d6 */
    unsigned char fn[] = {0x40, 0x05, 0x80, 0xd2, 0xc0, 0x03, 0x5f, 0xd6};
    memcpy(code_page, fn, sizeof(fn));
    if (mprotect(code_page, 4096, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect r-x");
        return 1;
    }

    signal(SIGUSR1, on_fork);
    signal(SIGUSR2, on_mprotect);

    printf("PID=%d CODE=%p\n", getpid(), code_page);
    fflush(stdout);

    for (;;)
        pause();
    return 0;
}
