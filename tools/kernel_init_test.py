#!/usr/bin/env python3
"""Fault-inject the production init function using host kernel API stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'lsdriver/lsdriver.c').read_text()
start = source.index('static int __init lsdriver_init(void)')
end = source.index('static void __exit lsdriver_exit', start)
init = source[start:end]
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define __init
#define MODULE
#define NULL ((void *)0)
#define IS_ERR(p) ((p) == &error_task)
#define PTR_ERR(p) (-12)
#define LS_LOG_ALWAYS_UNUSED 0
#define ls_log_always_tag(...) ((void)0)
struct task_struct { int pid; };
static struct task_struct tasks[2], error_task;
static struct task_struct *connect_thread_task, *dispatch_thread_task;
static int fault, creates, stopped, freed, hooks, wakes, hidden;
static int ConnectThreadFunction(void *p) { return 0; }
static int DispatchThreadFunction(void *p) { return 0; }
static void bypass_cfi(void) {}
static int allocate_physical_page_info(void) { return fault == 1 ? -12 : 0; }
static void free_physical_page_info(void) { ++freed; assert(!hooks && !wakes); }
static struct task_struct *kthread_create(int (*fn)(void *), void *data, const char *name) {
    assert(!hooks && !wakes);
    ++creates;
    if (fault == creates + 1) return &error_task;
    tasks[creates-1].pid = creates;
    return &tasks[creates-1];
}
static int kthread_stop(struct task_struct *t) { assert(!hooks && !wakes); ++stopped; return 0; }
static void sched_set_fifo_low(struct task_struct *t) {}
static void sched_set_fifo(struct task_struct *t) {}
static int proc_create_block_init(void) { ++hooks; return fault == 4 ? -95 : 0; }
static int do_exit_init(void) { ++hooks; return fault == 5 ? -95 : 0; }
static int hide_task_install(int pid) { ++hooks; return fault == 6 ? -95 : 0; }
static void hide_myself(void) { assert(hooks == 4 && !wakes && !freed); ++hidden; }
static void wake_up_process(struct task_struct *t) { assert(hidden && !freed); ++wakes; }
'''
main = r'''
int main(void) {
    for (fault = 0; fault <= 6; ++fault) {
        creates = stopped = freed = hooks = wakes = hidden = 0;
        connect_thread_task = dispatch_thread_task = NULL;
        int result = lsdriver_init();
        if (fault >= 1 && fault <= 3) {
            assert(result == -12 && freed == 1 && !hooks && !wakes && !hidden);
            assert(!connect_thread_task && !dispatch_thread_task);
            assert(stopped == (fault == 3 ? 1 : 0));
        } else {
            assert(result == 0 && wakes == 2 && hidden == 1 && !freed && !stopped);
        }
    }
    puts("Kernel init allocation/thread fault injection and nonfatal hook failures passed");
}
'''
with tempfile.TemporaryDirectory(prefix='lsdriver-init-test-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(stubs + init + main)
    subprocess.run(['cc', '-std=gnu11', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
