#ifndef CPUBIND_H
#define CPUBIND_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CPUBIND_ROLE_HI1 = 0,   /* 高优先级线程 A */
    CPUBIND_ROLE_HI2,       /* 高优先级线程 B */
    CPUBIND_ROLE_MAIN,      /* 主线程 */
    CPUBIND_ROLE_COUNT
} cpubind_role_t;

/**
 * 初始化：探测可用 CPU、采样负载、预分配三个角色的核。
 * 只在启动阶段调用一次（通常在主线程里，pthread_create 之前）。
 *
 * @return 0 成功，-1 失败（errno 有设置）
 */
int  cpubind_init(void);

/**
 * 关闭库，释放内部状态。一般不需要调用。
 */
void cpubind_shutdown(void);

/**
 * 绑定当前线程到指定角色分到的 CPU。
 * 若 init 时预分配的核绑定失败，会重新采样并选一个未占用的核。
 * 由目标线程自己调用（sched_setaffinity 是 per-thread 的）。
 *
 * @return 实际绑定的 CPU 编号（>=0），失败返回 -1
 */
int  cpubind_bind(cpubind_role_t role);

/**
 * 查询某角色已分到的 CPU（未分配返回 -1）。
 */
int  cpubind_get_cpu(cpubind_role_t role);

/**
 * 返回 init 时探测到的可用 CPU 数量。
 */
int  cpubind_available_count(void);

/* -------- 便捷封装 -------- */
#define cpubind_bind_hi1()   cpubind_bind(CPUBIND_ROLE_HI1)
#define cpubind_bind_hi2()   cpubind_bind(CPUBIND_ROLE_HI2)
#define cpubind_bind_main()  cpubind_bind(CPUBIND_ROLE_MAIN)

#ifdef __cplusplus
}
#endif

#endif /* CPUBIND_H */
