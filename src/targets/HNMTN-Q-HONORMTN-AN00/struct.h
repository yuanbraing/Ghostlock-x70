#include <stdint.h>
struct rb_node {
    unsigned long  __rb_parent_color;
    struct rb_node *rb_right;
    struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));

struct rt_mutex_waiter {
    struct rb_node tree_entry;
    struct rb_node pi_tree_entry;
    struct task_struct *task;
    struct rt_mutex_base *lock;
    unsigned int wake_state;
    int prio;
    uint64_t deadline;
    struct ww_acquire_ctx *ww_ctx;
};

struct file_operations {
        struct module *owner;
        uintptr_t llseek;
        uintptr_t read;
        uintptr_t write;
        uintptr_t read_iter;
        uintptr_t write_iter;
        uintptr_t iopoll;
        uintptr_t iterate;
        uintptr_t iterate_shared;
        uintptr_t poll;
        uintptr_t unlocked_ioctl;
        uintptr_t compat_ioctl;
        uintptr_t mmap;
        unsigned long mmap_supported_flags;                                                                                    uintptr_t open;
        uintptr_t flush;                                                                                                       uintptr_t release;
        uintptr_t fsync;
        uintptr_t fasync;
        uintptr_t lock;
        uintptr_t sendpage;
        uintptr_t get_unmapped_area;
        uintptr_t check_flags;
        uintptr_t flock;
        uintptr_t splice_write;
        uintptr_t splice_read;
        uintptr_t setlease;
        uintptr_t fallocate;
        uintptr_t show_fdinfo;
        uintptr_t copy_file_range;                                                                                             uintptr_t remap_file_range;
        uintptr_t fadvise;                                                                                                     uintptr_t uring_cmd;
        uintptr_t uring_cmd_iopoll;
};

struct pipe_buffer {
  uint64_t page;
  uint32_t offset;
  uint32_t len;
  uint64_t ops;
  uint32_t flags;
  uint32_t pad;
  uint64_t private;
};
