/* SPDX-License-Identifier: GPL-2.0 */
/*
 * module_lookup.h - 内核模块布局/导出符号定位辅助（头文件）
 *
 * 本文件与配套的 module_lookup.c 均不提供任何读原语、不含漏洞，
 * 仅作为安全研究辅助：给定内核任意读原语与 KASLR 转换函数，即可
 * 枚举模块、解析段布局、定位导出符号。
 *
 * 外部依赖（由使用者提供）：
 *   u64 module_read(u64 addr);
 *       一次性读取 8 字节，小端解包。
 *
 *   u64 text_addr(u64 image);
 *       内核 image 内的链接地址/偏移 -> KASLR 后的运行时虚拟地址。
 *       仅对内核镜像本体（text/data）有效；模块内存（vmalloc）本身
 *       已是运行时地址，不再经过此函数。
 *
 * 校准：
 *   $ pahole -C module vmlinux                    # 取 list/name/syms/...
 *   $ pahole -C module_layout vmlinux             # 校验 module_layout 字段
 *   $ readelf -sW vmlinux | grep -w modules       # 取 modules 的链接地址
 *   MODULES_IMAGE_OFF = modules_link_addr - _text_link_addr（或直接填链接地址，
 *   视 text_addr 的语义而定，见下方注释）
 */

#ifndef MODULE_LOOKUP_H
#define MODULE_LOOKUP_H

#include <stdint.h>
#include <stddef.h>

/* ---------------- 基础类型 ---------------- */
typedef uint64_t u64;
typedef uint32_t u32;
typedef int64_t  s64;
typedef int32_t  s32;

/* ---------------- 外部依赖 ---------------- */
extern u64 module_read(u64 addr);   /* 一次读 8 字节 */
extern u64 text_addr(u64 image);    /* image 链接地址 -> 运行时地址 */

/* ================================================================== */
/*  偏移宏 —— 必须按目标内核（同一份 vmlinux）校准                      */
/* ================================================================== */

/* struct module 字段偏移 */
#define MOD_STATE_OFF            0x0000
#define MOD_LIST_OFF            0x0008  /* struct list_head list        */
#define MOD_NAME_OFF            0x0018  /* char name[MODULE_NAME_LEN]   */
#define MOD_SYMS_OFF            0x00F8  /* const struct kernel_symbol * */
#define MOD_NUM_SYMS_OFF        0x0108  /* unsigned int num_syms        */
#define MOD_NUM_GPL_OFF         0x011C  /* unsigned int num_gpl_syms    */
#define MOD_GPL_SYMS_OFF        0x014C  /* const struct kernel_symbol * */

#define MOD_VERSION_OFF 0xd8
#define MOD_SRCVERSION_OFF 0xe0
#define MOD_SCMVERSION_OFF 0xe8

/* struct module_layout 字段偏移 */
#define ML_BASE_OFF              0x00
#define ML_SIZE_OFF              0x08
#define ML_TEXT_SIZE_OFF         0x0C
#define ML_RO_SIZE_OFF           0x10
#define ML_RO_AFTER_INIT_SIZE_OFF 0x14

/* struct module 内嵌布局结构的偏移 */
#define MOD_CORE_LAYOUT_OFF      0x180
#define MOD_INIT_LAYOUT_OFF      0x1D0
/* #define MOD_DATA_LAYOUT_OFF   <未找到：目标内核未启用 DATA_IN_VMALLOC> */
#define MODULE_LAYOUT_SIZE       0x50

/* 命名与符号尺寸 */
#define MOD_NAME_LEN             64
/* CONFIG_HAVE_ARCH_PREL32_RELOCATIONS=y 时，kernel_symbol = 3 * int32 */
#define KSYM_SIZE                12

/*
 * 内核全局 `modules` 的链接地址（或 image 偏移，取决于 text_addr 语义）。
 * 直接填 vmlinux 里 `modules` 的链接地址，由 text_addr() 完成 KASLR 转换。
 */
#ifndef MODULES_IMAGE_OFF
#define MODULES_IMAGE_OFF        0xffffffc00a45d970ULL
#endif

/* 内核地址下界，用于剔除明显非法的解码结果 */
#define KERN_ADDR_MIN            0xffff800000000000ULL

/* 模块链表遍历最大步数，防环 */
#define MOD_LIST_MAX_STEPS       8192

/* ================================================================== */
/*  数据类型                                                           */
/* ================================================================== */

/* 单个 struct module_layout 的解码结果 */
struct mod_layout {
    u64 base;                 /* 映射基址（运行时）          */
    u32 size;                 /* 布局总大小                  */
    u32 text_size;            /* text 段末（相对 base）      */
    u32 ro_size;              /* 只读段末（相对 base）       */
    u32 ro_after_init_size;   /* init 后只读段末（相对 base）*/
};

/* 段类别，用于 module_section_range() / module_addr_section() */
enum mod_section {
    MOD_SEC_TEXT = 0,         /* .text                     */
    MOD_SEC_RODATA,           /* .rodata                   */
    MOD_SEC_RO_AFTER_INIT,    /* .ro_after_init            */
    MOD_SEC_DATA,             /* .data / .bss              */
    MOD_SEC_INIT,             /* __init / __exit           */
};

enum mod_state {
    MOD_STATE_LIVE     = 0,
    MOD_STATE_COMING   = 1,
    MOD_STATE_GOING    = 2,
    MOD_STATE_UNFORMED = 3,
};


/* ================================================================== */
/*  对外 API                                                           */
/* ================================================================== */

/*
 * 按名称查找已加载模块，成功返回 struct module * 的运行时地址。
 * 未找到或参数非法返回 0。
 */
u64 module_find_by_name(const char *name);

/*
 * 在给定 struct module 中查找导出符号（__ksymtab / __ksymtab_gpl）。
 * 命中返回符号运行时地址；未找到或参数非法返回 0。
 * 注意：只覆盖导出符号，模块内部静态符号不在其中。
 */
u64 module_lookup_export(u64 mod, const char *sym);

/*
 * 一站式：模块名 + 导出符号名 -> 运行时地址。
 * 任一步失败返回 0。
 */
u64 module_resolve(const char *mod_name, const char *sym);

/*
 * 解码模块的 core / init 布局。
 * @core/@init 可为 NULL 表示不关心。
 * 返回 0 成功；@init->size == 0 表示模块无 __init/__exit。
 */
int module_get_layouts(u64 mod,
                       struct mod_layout *core,
                       struct mod_layout *init);

/*
 * 由 core/init 布局反推某段的运行时 [start, end)。
 * 成功返回 0；段不存在返回 -ENOENT；参数非法返回 -EINVAL。
 */
int module_section_range(u64 mod, int which, u64 *start, u64 *end);

/*
 * 判断运行时地址落在模块哪个段。
 * 返回 enum mod_section；不属于该模块返回 -1。
 */
int module_addr_section(u64 mod, u64 addr);

/* 读模块 state 字段；失败返回 -1 */
int module_get_state(u64 mod);

/* 返回 "live" / "coming" / "going" / "unformed" / "unknown" */
const char *module_state_name(int state);

/*
 * 读模块的某个 modinfo 字符串字段：
 *   @field_off 为 MOD_VERSION_OFF / MOD_SRCVERSION_OFF / MOD_SCMVERSION_OFF，
 *   也可用于任何 char * 字段。
 * 成功返回 0；
 * 字段指针为空或非法返回 -ENOENT；
 * 参数非法返回 -EINVAL。
 */
int module_get_modinfo_field(u64 mod, u32 field_off, char *buf, size_t max);

int module_get_version(u64 mod, char *buf, size_t max);
int module_get_srcversion(u64 mod, char *buf, size_t max);
int module_get_scmversion(u64 mod, char *buf, size_t max);

#endif /* MODULE_LOOKUP_H */
