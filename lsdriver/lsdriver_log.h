#ifndef LSDRIVER_LOG_H
#define LSDRIVER_LOG_H

#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/string.h>

// 只缓存本驱动的两类监控日志，不向应用开放系统 dmesg 权限。
#define LS_MONITOR_LOG_BYTES 16384
struct ls_monitor_log_buffer {
    char data[LS_MONITOR_LOG_BYTES];
    unsigned int head;
    unsigned int count;
};
static struct ls_monitor_log_buffer g_ls_monitor_logs[2];
static DEFINE_SPINLOCK(g_ls_monitor_log_lock);

static int ls_monitor_log_index(const char *tag)
{
    if (!strcmp(tag, "sysmon")) return 0;
    if (!strcmp(tag, "cntvct")) return 1;
    return -1;
}

static void ls_monitor_log_append(int index, const char *text, size_t size)
{
    struct ls_monitor_log_buffer *log = &g_ls_monitor_logs[index];
    unsigned long flags;
    spin_lock_irqsave(&g_ls_monitor_log_lock, flags);
    for (size_t i = 0; i < size; i++) {
        log->data[log->head] = text[i];
        log->head = (log->head + 1) % LS_MONITOR_LOG_BYTES;
        if (log->count < LS_MONITOR_LOG_BYTES) log->count++;
    }
    spin_unlock_irqrestore(&g_ls_monitor_log_lock, flags);
}

static int ls_monitor_log_read(uint64_t selector, void *output, int capacity)
{
    struct ls_monitor_log_buffer *log;
    unsigned int size, start;
    unsigned long flags;
    char *dst = output;
    if (selector < 1 || selector > 2 || !output || capacity <= 0 || capacity > 4096) return -EINVAL;
    log = &g_ls_monitor_logs[selector - 1];
    spin_lock_irqsave(&g_ls_monitor_log_lock, flags);
    size = min_t(unsigned int, log->count, capacity);
    start = (log->head + LS_MONITOR_LOG_BYTES - size) % LS_MONITOR_LOG_BYTES;
    // 只返回完整日志行，避免从被截断的行中间开始显示。
    if (size < log->count) {
        while (size && log->data[start] != '\n') {
            start = (start + 1) % LS_MONITOR_LOG_BYTES;
            size--;
        }
        if (size) { start = (start + 1) % LS_MONITOR_LOG_BYTES; size--; }
    }
    for (unsigned int i = 0; i < size; i++) dst[i] = log->data[(start + i) % LS_MONITOR_LOG_BYTES];
    spin_unlock_irqrestore(&g_ls_monitor_log_lock, flags);
    return size;
}

#define ls_log(fmt, ...)          pr_debug("lsdriver: " fmt, ##__VA_ARGS__)
#define ls_log_tag(tag, fmt, ...) pr_debug("lsdriver: %s: " fmt, tag, ##__VA_ARGS__)

// 直接以最高级别KERN_EMERG 写入 printk 环形缓冲区，不依赖 DEBUG 或 dynamic_debug，适合必须保留的监控日志；
static __printf(2, 3) void ls_log_always_tag(const char *tag, const char *fmt, ...)
{
    struct va_format vaf;
    va_list args;
    va_list copy;
    char line[512];
    int index, length;

    va_start(args, fmt);
    index = ls_monitor_log_index(tag);
    if (index >= 0) {
        length = scnprintf(line, sizeof(line), "lsdriver: %s: ", tag);
        va_copy(copy, args);
        length += vscnprintf(line + length, sizeof(line) - length, fmt, copy);
        va_end(copy);
        if (length && line[length - 1] != '\n') line[length++] = '\n';
        ls_monitor_log_append(index, line, length);
    }
    vaf.fmt = fmt;
    vaf.va = &args;
    printk(KERN_EMERG "lsdriver: %s: %pV", tag, &vaf);
    va_end(args);
}

#endif // LSDRIVER_LOG_H
