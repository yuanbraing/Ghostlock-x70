#define _GNU_SOURCE
#include "cpubind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <pthread.h>
#include <sys/sysinfo.h>

#define PROC_STAT            "/proc/stat"
#define CORE_CTL_ISOLATED    "/sys/devices/system/cpu/core_ctl_isolated"
#define SAMPLE_INTERVAL_US   100000     /* 100ms */
#define MAX_BIND_ATTEMPTS    5
#define MAX_CPUS             CPU_SETSIZE

/* ---------------- 内部类型 ---------------- */

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    unsigned long long total;
} CpuJiffies;

typedef struct { int cpu; double load; } CpuLoad;

typedef struct {
    int  initialized;
    int  verbose;
    int  num_cpus;
    int  isolated[MAX_CPUS];
    int  usable[MAX_CPUS];
    int  assigned[MAX_CPUS];
    int  role_cpu[CPUBIND_ROLE_COUNT];
    pthread_mutex_t lock;
} cpubind_state_t;

static cpubind_state_t g = {
    .initialized = 0,
    .lock        = PTHREAD_MUTEX_INITIALIZER,
};

/* ---------------- 内部工具 ---------------- */

static void dbg(const char *fmt, ...) {
    if (!g.verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static int read_proc_stat(CpuJiffies *stats) {
    FILE *fp = fopen(PROC_STAT, "r");
    if (!fp) return -1;

    char line[256];
    int cpu_count = 0;
    memset(stats, 0, sizeof(CpuJiffies) * MAX_CPUS);

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0) continue;
        if (line[3] < '0' || line[3] > '9') continue;

        int cpu_id;
        unsigned long long u, n, s, i, io, ir, si, st;
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                   &cpu_id, &u, &n, &s, &i, &io, &ir, &si, &st) < 8)
            continue;
        if (cpu_id < 0 || cpu_id >= MAX_CPUS) continue;

        CpuJiffies *p = &stats[cpu_id];
        p->user = u; p->nice = n; p->system = s; p->idle = i;
        p->iowait = io; p->irq = ir; p->softirq = si; p->steal = st;
        p->total = u + n + s + i + io + ir + si + st;
        if (cpu_id + 1 > cpu_count) cpu_count = cpu_id + 1;
    }
    fclose(fp);
    return cpu_count;
}

static double calc_cpu_load(const CpuJiffies *p, const CpuJiffies *c) {
    unsigned long long td = c->total - p->total;
    if (td == 0) return 0.0;
    unsigned long long id = (c->idle + c->iowait) - (p->idle + p->iowait);
    if (id > td) id = td;
    return (double)(td - id) / (double)td;
}

static int read_core_ctl_isolated(int *mask) {
    memset(mask, 0, sizeof(int) * MAX_CPUS);

    FILE *fp = fopen(CORE_CTL_ISOLATED, "r");
    if (!fp) return (errno == ENOENT) ? 0 : -1;

    char buf[1024] = {0};
    if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); return 0; }
    fclose(fp);

    char *tok = strtok(buf, ", \t\n");
    while (tok) {
        int a, b;
        if (sscanf(tok, "%d-%d", &a, &b) == 2) {
            for (int i = a; i <= b && i < MAX_CPUS; i++)
                if (i >= 0) mask[i] = 1;
        } else {
            int c = atoi(tok);
            if (c >= 0 && c < MAX_CPUS) mask[c] = 1;
        }
        tok = strtok(NULL, ", \t\n");
    }
    return 0;
}

static int cmp_load_asc(const void *a, const void *b) {
    const CpuLoad *x = a, *y = b;
    if (x->load < y->load) return -1;
    if (x->load > y->load) return  1;
    return 0;
}

/* 探测：对每个候选 CPU 试一次 setaffinity，成功则记为 usable。
 * 调用时持有 g.lock。 */
static void probe_usable(void) {
    memset(g.usable, 0, sizeof(g.usable));

    cpu_set_t allowed;
    int have_allowed = (sched_getaffinity(0, sizeof(allowed), &allowed) == 0);

    int ok = 0, dead = 0, iso = 0, forbidden = 0;

    for (int i = 0; i < g.num_cpus; i++) {
        if (g.isolated[i]) { iso++; continue; }
        if (have_allowed && !CPU_ISSET(i, &allowed)) { forbidden++; continue; }

        cpu_set_t m;
        CPU_ZERO(&m);
        CPU_SET(i, &m);
        if (sched_setaffinity(0, sizeof(m), &m) == -1) { dead++; continue; }

        g.usable[i] = 1;
        ok++;
    }

    /* 恢复原来的亲和性 */
    if (have_allowed) sched_setaffinity(0, sizeof(allowed), &allowed);

    dbg("[cpubind] 探测：可用 %d，隔离 %d，cpuset 外 %d，EINVAL %d\n",
        ok, iso, forbidden, dead);
}

/* 采样一轮，返回按负载升序的可用 CPU 列表（跳过 blocked）。
 * 调用时持有 g.lock。 */
static int sample_ranked(int *out, const int *blocked) {
    CpuJiffies prev[MAX_CPUS], curr[MAX_CPUS];

    if (read_proc_stat(prev) < 0) return 0;
    usleep(SAMPLE_INTERVAL_US);
    if (read_proc_stat(curr) < 0) return 0;

    CpuLoad list[MAX_CPUS];
    int n = 0;
    for (int i = 0; i < g.num_cpus; i++) {
        if (!g.usable[i]) continue;
        if (blocked && blocked[i]) continue;
        list[n].cpu  = i;
        list[n].load = calc_cpu_load(&prev[i], &curr[i]);
        n++;
    }
    qsort(list, n, sizeof(CpuLoad), cmp_load_asc);
    for (int i = 0; i < n; i++) out[i] = list[i].cpu;
    return n;
}

/* 把当前线程绑到 cpu。成功返回 cpu，失败返回 -1（errno 保留）。 */
static int do_bind(int cpu) {
    cpu_set_t m;
    CPU_ZERO(&m);
    CPU_SET(cpu, &m);
    if (sched_setaffinity(0, sizeof(m), &m) == -1) return -1;
    return cpu;
}

/* ---------------- 公开 API ---------------- */

int cpubind_init(void) {
    pthread_mutex_lock(&g.lock);

    if (g.initialized) {
        pthread_mutex_unlock(&g.lock);
        return 0;
    }

    const char *v = getenv("CPUBIND_DEBUG");
    g.verbose = (v && *v && *v != '0');

    memset(g.usable,   0, sizeof(g.usable));
    memset(g.assigned, 0, sizeof(g.assigned));
    memset(g.isolated, 0, sizeof(g.isolated));
    for (int i = 0; i < CPUBIND_ROLE_COUNT; i++) g.role_cpu[i] = -1;

    g.num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (g.num_cpus <= 0) {
        pthread_mutex_unlock(&g.lock);
        return -1;
    }
    if (g.num_cpus > MAX_CPUS) g.num_cpus = MAX_CPUS;
      cpu_set_t mask;
  CPU_ZERO(&mask);                 // 先清空
  memset(&mask, 0xFF, sizeof(cpu_set_t)); // 将所有位置 1，即允许所有 CPU
  
  // pid 传 0 表示当前线程
  if (sched_setaffinity(0, sizeof(cpu_set_t), &mask) != 0) {
      perror("sched_setaffinity");
      return -1;
  }
    if (read_core_ctl_isolated(g.isolated) != 0) {
        pthread_mutex_unlock(&g.lock);
        return -1;
    }

    probe_usable();

    /* 采样一次，预选前 N 个负载最低的核 */
    int ranked[MAX_CPUS];
    int n = sample_ranked(ranked, NULL);
    if (n < CPUBIND_ROLE_COUNT) {
        dbg("[cpubind] 可用 CPU 不足 %d 个（只有 %d 个）\n",
            CPUBIND_ROLE_COUNT, n);
        pthread_mutex_unlock(&g.lock);
        errno = ENOSPC;
        return -1;
    }

    for (int i = 0; i < CPUBIND_ROLE_COUNT; i++) {
        g.role_cpu[i]        = ranked[i];
        g.assigned[ranked[i]] = 1;
        dbg("[cpubind] 角色 %d 预分配 CPU %d\n", i, ranked[i]);
    }

    g.initialized = 1;
    pthread_mutex_unlock(&g.lock);
    return 0;
}

void cpubind_shutdown(void) {
    pthread_mutex_lock(&g.lock);
    g.initialized = 0;
    for (int i = 0; i < CPUBIND_ROLE_COUNT; i++) g.role_cpu[i] = -1;
    memset(g.usable,   0, sizeof(g.usable));
    memset(g.assigned, 0, sizeof(g.assigned));
    pthread_mutex_unlock(&g.lock);
}

int cpubind_bind(cpubind_role_t role) {
    if (role < 0 || role >= CPUBIND_ROLE_COUNT) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g.lock);
    if (!g.initialized) {
        pthread_mutex_unlock(&g.lock);
        errno = EINVAL;
        return -1;
    }

    int target = g.role_cpu[role];

    /* 快速路径：直接绑预分配的核 */
    if (target >= 0) {
        if (do_bind(target) >= 0) {
            pthread_mutex_unlock(&g.lock);
            return target;
        }

        /* 预分配失败：释放该核，进入重测流程 */
        int saved = errno;
        dbg("[cpubind] 角色 %d 绑 CPU %d 失败（%s），重新采样\n",
            role, target, strerror(saved));

        g.assigned[target] = 0;
        g.role_cpu[role]   = -1;
        errno = saved;
    }

    /* 慢速路径：重测，跳过所有已分配出去的核 */
    int blocked[MAX_CPUS] = {0};
    for (int i = 0; i < MAX_CPUS; i++)
        if (g.assigned[i]) blocked[i] = 1;
    if (target >= 0) blocked[target] = 1;

    for (int att = 0; att < MAX_BIND_ATTEMPTS; att++) {
        int ranked[MAX_CPUS];
        int n = sample_ranked(ranked, blocked);
        if (n <= 0) break;

        int new_cpu = ranked[0];
        if (do_bind(new_cpu) >= 0) {
            g.assigned[new_cpu] = 1;
            g.role_cpu[role]    = new_cpu;
            dbg("[cpubind] 角色 %d 重测后选 CPU %d\n", role, new_cpu);
            pthread_mutex_unlock(&g.lock);
            return new_cpu;
        }
        blocked[new_cpu] = 1;
    }

    pthread_mutex_unlock(&g.lock);
    return -1;
}

int cpubind_get_cpu(cpubind_role_t role) {
    if (role < 0 || role >= CPUBIND_ROLE_COUNT) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g.lock);
    int c = g.initialized ? g.role_cpu[role] : -1;
    pthread_mutex_unlock(&g.lock);
    return c;
}

int cpubind_available_count(void) {
    pthread_mutex_lock(&g.lock);
    int n = 0;
    if (g.initialized)
        for (int i = 0; i < g.num_cpus; i++) n += g.usable[i] ? 1 : 0;
    pthread_mutex_unlock(&g.lock);
    return n;
}
