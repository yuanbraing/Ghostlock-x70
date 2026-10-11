// SPDX-License-Identifier: GPL-2.0
/*
 * module_lookup.c - 内核模块布局/导出符号定位辅助（实现）
 *
 * 只依赖 module_lookup.h 中声明的两个外部函数：
 *   module_read() / text_addr()
 * 本文件不提供任何原语、不含漏洞。
 */

#include <string.h>
#include <errno.h>

#include "module.h"

/* ================================================================== */
/*  基于 8 字节读的子字 / 字符串读取                                    */
/* ================================================================== */

static inline u64 mod_rd64(u64 a)
{
    return module_read(a);
}

/* 读 32 位，借一次 8 字节读 + 移位，兼容非对齐地址 */
static inline u32 mod_rd32(u64 a)
{
    u64 base = a & ~7ULL;
    u64 v    = module_read(base);
    return (u32)(v >> ((a - base) * 8));
}

static inline s32 mod_rd_s32(u64 a)
{
    return (s32)mod_rd32(a);
}

/*
 * 读 NUL 结尾字符串：每次 8 字节，逐字节解包，
 * 直到遇到 '\0' 或写满 buf。返回 0 表示正常终止。
 */
static int mod_rd_cstr(u64 a, char *buf, size_t max)
{
    size_t i = 0;

    if (!buf || max < 2)
        return -EINVAL;

    while (i + 1 < max) {
        u64 aligned = (a + i) & ~7ULL;
        u64 v       = module_read(aligned);
        unsigned sh = (unsigned)((a + i) - aligned) * 8;

        while (sh < 64 && i + 1 < max) {
            char c = (char)(v >> sh);
            buf[i++] = c;
            if (c == '\0')
                return 0;
            sh += 8;
        }
    }
    buf[max - 1] = '\0';
    return -ENAMETOOLONG;
}

/* ================================================================== */
/*  PREL32 kernel_symbol 解码                                          */
/* ================================================================== */

/*
 * struct kernel_symbol {
 *     int value_offset;     // 相对本字段地址
 *     int name_offset;      // 相对本字段地址
 *     int namespace_offset;
 * };
 */
static int mod_decode_ksym(u64 ksym, u64 *value, u64 *name)
{
    s32 voff = mod_rd_s32(ksym + 0);
    s32 noff = mod_rd_s32(ksym + 4);
    u64 v    = (u64)((s64)ksym + voff);
    u64 n    = (u64)((s64)(ksym + 4) + noff);

    if (v < KERN_ADDR_MIN || n < KERN_ADDR_MIN)
        return -EINVAL;

    *value = v;
    *name  = n;
    return 0;
}

/* ================================================================== */
/*  扫描单个 ksymtab                                                   */
/* ================================================================== */

static int mod_scan_ksymtab(u64 syms, u32 num,
                            const char *target, u64 *out)
{
    u32 i;

    if (!syms || !num || !target || !out)
        return -EINVAL;

    for (i = 0; i < num; i++) {
        u64 ksym = syms + (u64)i * KSYM_SIZE;
        u64 value, name;
        char buf[256];

        if (mod_decode_ksym(ksym, &value, &name) < 0)
            continue;
        if (mod_rd_cstr(name, buf, sizeof(buf)) < 0)
            continue;
        if (strcmp(buf, target) == 0) {
            *out = value;
            return 0;
        }
    }
    return -ENOENT;
}

/* ================================================================== */
/*  API：模块查找                                                      */
/* ================================================================== */

u64 module_find_by_name(const char *name)
{
    u64 head, curr;
    int guard = 0;

    if (!name)
        return 0;

    head = text_addr(MODULES_IMAGE_OFF);
    if (head < KERN_ADDR_MIN)
        return 0;

    curr = mod_rd64(head);          /* head->next */
    while (curr && curr != head && guard++ < MOD_LIST_MAX_STEPS) {
        u64 mod = curr - MOD_LIST_OFF;
        char mname[MOD_NAME_LEN + 1];

        if (mod_rd_cstr(mod + MOD_NAME_OFF, mname, sizeof(mname)) == 0
            && strcmp(mname, name) == 0)
            return mod;

        curr = mod_rd64(curr);      /* list.next */
    }
    return 0;
}

/* ================================================================== */
/*  API：导出符号查找                                                  */
/* ================================================================== */

u64 module_lookup_export(u64 mod, const char *sym)
{
    u64 syms, gpl_syms, out = 0;
    u32 num, num_gpl;

    if (!mod || !sym)
        return 0;

    syms     = mod_rd64(mod + MOD_SYMS_OFF);
    num      = mod_rd32(mod + MOD_NUM_SYMS_OFF);
    gpl_syms = mod_rd64(mod + MOD_GPL_SYMS_OFF);
    num_gpl  = mod_rd32(mod + MOD_NUM_GPL_OFF);

    if (mod_scan_ksymtab(syms, num, sym, &out) == 0)
        return out;
    if (mod_scan_ksymtab(gpl_syms, num_gpl, sym, &out) == 0)
        return out;
    return 0;
}

u64 module_resolve(const char *mod_name, const char *sym)
{
    u64 mod = module_find_by_name(mod_name);

    if (!mod)
        return 0;
    return module_lookup_export(mod, sym);
}

/* ================================================================== */
/*  API：布局解码                                                      */
/* ================================================================== */

static void mod_read_layout(u64 addr, struct mod_layout *out)
{
    if (!out)
        return;

    out->base               = mod_rd64(addr + ML_BASE_OFF);
    out->size               = mod_rd32(addr + ML_SIZE_OFF);
    out->text_size          = mod_rd32(addr + ML_TEXT_SIZE_OFF);
    out->ro_size            = mod_rd32(addr + ML_RO_SIZE_OFF);
    out->ro_after_init_size = mod_rd32(addr + ML_RO_AFTER_INIT_SIZE_OFF);
}

int module_get_layouts(u64 mod,
                       struct mod_layout *core,
                       struct mod_layout *init)
{
    if (!mod)
        return -EINVAL;

    if (core)
        mod_read_layout(mod + MOD_CORE_LAYOUT_OFF, core);
    if (init)
        mod_read_layout(mod + MOD_INIT_LAYOUT_OFF, init);
    /* 目标内核未启用 CONFIG_ARCH_WANTS_MODULES_DATA_IN_VMALLOC，
     * 无独立 data_layout；rw 段位于 core_layout 末尾。 */
    return 0;
}

int module_section_range(u64 mod, int which, u64 *start, u64 *end)
{
    struct mod_layout core = {0}, init = {0};
    u64 b;

    if (!mod || !start || !end)
        return -EINVAL;

    module_get_layouts(mod, &core, &init);
    if (!core.base || !core.size)
        return -EINVAL;

    b = core.base;
    switch (which) {
    case MOD_SEC_TEXT:
        *start = b;
        *end   = b + core.text_size;
        break;
    case MOD_SEC_RODATA:
        *start = b + core.text_size;
        *end   = b + core.ro_size;
        break;
    case MOD_SEC_RO_AFTER_INIT:
        *start = b + core.ro_size;
        *end   = b + core.ro_after_init_size;
        break;
    case MOD_SEC_DATA:
        *start = b + core.ro_after_init_size;
        *end   = b + core.size;
        break;
    case MOD_SEC_INIT:
        if (!init.size)
            return -ENOENT;
        *start = init.base;
        *end   = init.base + init.size;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

int module_addr_section(u64 mod, u64 addr)
{
    u64 s, e;
    int i;

    for (i = 0; i <= MOD_SEC_INIT; i++) {
        if (module_section_range(mod, i, &s, &e) == 0
            && addr >= s && addr < e)
            return i;
    }
    return -1;
}
/* ================================================================== */
/*  API：模块状态与 modinfo 字符串                                     */
/* ================================================================== */

int module_get_state(u64 mod)
{
    if (!mod)
        return -1;
    return (int)mod_rd32(mod + MOD_STATE_OFF);
}

const char *module_state_name(int state)
{
    switch (state) {
    case MOD_STATE_LIVE:     return "live";
    case MOD_STATE_COMING:   return "coming";
    case MOD_STATE_GOING:    return "going";
    case MOD_STATE_UNFORMED: return "unformed";
    default:                 return "unknown";
    }
}

/*
 * 通用：读 struct module 里某个 char * 字段所指向的字符串。
 * 先取指针、做内核地址下界校验，再逐字节读字符串。
 */
int module_get_modinfo_field(u64 mod, u32 field_off, char *buf, size_t max)
{
    u64 ptr;

    if (!mod || !buf || max < 2)
        return -EINVAL;
    if (field_off == 0)
        return -ENOENT;   /* 未校准/该内核无此字段 */

    ptr = mod_rd64(mod + field_off);
    if (!ptr || ptr < KERN_ADDR_MIN)
        return -ENOENT;

    return mod_rd_cstr(ptr, buf, max);
}

int module_get_version(u64 mod, char *buf, size_t max)
{
    return module_get_modinfo_field(mod, MOD_VERSION_OFF, buf, max);
}

int module_get_srcversion(u64 mod, char *buf, size_t max)
{
    return module_get_modinfo_field(mod, MOD_SRCVERSION_OFF, buf, max);
}

int module_get_scmversion(u64 mod, char *buf, size_t max)
{
    return module_get_modinfo_field(mod, MOD_SCMVERSION_OFF, buf, max);
}
