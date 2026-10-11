HNMTN-Q-HONORMTN-AN00 内核提权与 KernelSU 集成工具

简介

本项目是一个面向 荣耀 X70（HNMTN-Q / HONORMTN-AN00） Android 16 设备的安全研究工具。它实现了完整的内核提权链，并支持安装 KernelSU 或传统 su 守护进程。核心流程包括：独立 KASLR 破解、KernelSnitch 泄漏 mm_struct、futex PI 漏洞利用、内核任意读写、cred 修补、SELinux/seccomp 绕过，最终在 root 子进程中部署所选方案。

免责声明
本项目仅供安全研究、漏洞分析和授权测试使用。请勿在您不拥有或未获得明确授权的设备上运行。作者不对任何滥用、设备损坏或数据丢失负责。使用前请确保您了解相关法律法规。

---

特性

· 独立 KASLR 破解：通过 perf_event_open 采样内核调用链，低 21 位匹配 + 多数投票推断 KASLR 偏移。
· KernelSnitch：利用 futex 哈希碰撞暴力搜索当前进程的 mm_struct 地址。
· GhostLock 风格 futex PI 利用：通过 FUTEX_CMP_REQUEUE_PI、信号栈伪造、rt_sigreturn 等实现内核栈劫持。
· 内核任意读写：结合 sysctl 原语与伪造 pipe_buffer 实现物理地址读写。
· 完整提权：修补当前进程 cred（UID/GID=0、Capabilities、SELinux SID）、清除 seccomp、关闭 SELinux enforcing。
· 双模式 Root 安装：
  · DIRECT_KSU=1：嵌入 assets/ksud，通过 ksud late-load 加载 KernelSU。
  · DIRECT_KSU=0：构建并安装传统 su 守护进程，可选安装壁纸。
· 反 Root 模块绕过：可禁用 rscan、kernel_stp 等内核模块的检测逻辑。
· CPU 绑定与负载均衡：cpubind 模块自动选择低负载核心，提高利用稳定性。

---

项目结构

```
.
├── Makefile                     # 构建入口
├── assets/
│   ├── ksud                     # KernelSU 用户态守护进程（DIRECT_KSU=1）
│   └── wallpaper.webp           # 可选壁纸（DIRECT_KSU=0）
├── src/
│   ├── main.c                   # 利用主流程、GhostLock 逻辑
│   ├── kaslr_crack.c            # perf 采样推断 KASLR 偏移
│   ├── cpubind.c / cpubind.h    # CPU 亲和性与负载均衡
│   ├── pipe.c                   # pipe_buffer 物理读写原语
│   ├── module.c / module.h      # 内核模块枚举与符号解析
│   ├── root.c                   # 提权、cred 修补、root 子进程
│   ├── preload.c                # 嵌入 su/ksud 与壁纸安装
│   ├── su_daemon.c              # 传统 su 守护进程（DIRECT_KSU=0）
│   ├── util.c                   # 辅助函数、ashmem 路径、内核读写封装
│   ├── poll.S                   # ppoll 系统调用跳板
│   ├── ksu_blob.S               # 嵌入 ksud 二进制
│   ├── su_blob.S                # 嵌入 su_daemon 二进制
│   ├── wallpaper_blob.S         # 嵌入壁纸
│   ├── common.h                 # 公共头文件
│   ├── offset.h                 # 目标配置转发
│   ├── kernelsnitch/            # KernelSnitch 实现
│   └── targets/
│       └── HNMTN-Q-HONORMTN-AN00/
│           ├── target.h         # 目标专用偏移与常量
│           └── struct.h         # 目标专用结构体定义
└── build/                       # 构建输出（gitignore）
```

---

构建要求

· Linux 或 Termux 构建环境（支持在 Termux 中直接构建）
· Android NDK r29 或兼容的 clang 工具链
    可通过 ANDROID_NDK_HOME、ANDROID_NDK_ROOT 指定，或放置于 ~/androids/ndk-r29
· GNU Make
· 目标设备内核偏移已在校准文件 src/targets/HNMTN-Q-HONORMTN-AN00/target.h 与 struct.h 中配置

在 Termux 中，可安装 clang、make、binutils、git 等包进行构建。若使用 NDK，请确保 NDK_ROOT 指向正确的 NDK 目录。

---

构建

```bash
# 默认构建 KernelSU 模式
make PROJECT=HNMTN-Q-HONORMTN-AN00 DIRECT_KSU=1

# 构建传统 su 模式（含壁纸）
make PROJECT=HNMTN-Q-HONORMTN-AN00 DIRECT_KSU=0

# 查看当前配置
make info

# 列出所有支持的目标
make list-projects
```

输出文件位于：

```
build/HNMTN-Q-HONORMTN-AN00/bin/preload
```

常用变量

变量 默认值 说明
API 35 Android API 级别
PROJECT HNMTN-Q-HONORMTN-AN00 目标项目名
OUTDIR build/$(PROJECT)/bin 输出目录
EMBEDDIR build/embed 嵌入二进制中间目录
DIRECT_KSU 1 1=KernelSU，0=传统 su
CC 自动探测 自定义编译器
NDK_ROOT 自动探测 NDK 根目录
EXTRA_CFLAGS 空 额外编译标志

---

部署与运行

```bash
# 推送并运行（自动 adb push + adb shell）
make run

# 仅推送
make install

# 手动运行
adb push build/HNMTN-Q-HONORMTN-AN00/bin/preload /data/local/tmp/
adb shell /data/local/tmp/preload
```

运行后，preload 会尝试完成提权并安装所选 Root 方案。成功时输出 SUCCESS，失败输出 FAILED。

---

工作原理（简述）

1. 初始化：探测 ashmem 设备路径，设置 CPU 绑定。
2. KASLR 破解：通过 perf_event_open 采集内核调用链，投票推断 KASLR 偏移。
3. KernelSnitch：利用 futex 哈希碰撞定位当前进程 mm_struct。
4. GhostLock 利用：通过 FUTEX_CMP_REQUEUE_PI 与信号栈伪造，劫持内核栈并建立读写原语。
5. 内核读写：
   · 使用 sysctl 原语读取 task_struct、files_struct、fdtable、file、pipe_inode_info。
   · 伪造 pipe_buffer，实现物理地址读写。
6. 提权：
   · 遍历任务链表找到 root 子进程。
   · 修补 cred：UID/GID 置 0，Capabilities 置满，SELinux SID 设为内核 SID。
   · 清除 TIF_SECCOMP、PFA_NO_NEW_PRIVS，清空 seccomp 过滤器。
   · 直接写入 SELinux enforcing 为 0。
7. Root 安装：
   · DIRECT_KSU=1：将嵌入的 ksud 写入 /data/local/tmp/ksud，执行 ksud late-load。
   · DIRECT_KSU=0：将嵌入的 su 写入 /apex/com.android.virt/lib64/su，启动 su --daemon，并可选安装壁纸。
8. 清理：关闭 sysctl 原语、释放管道、唤醒 waiter 线程。

---

目标设备

· 设备：荣耀 X70（HNMTN-Q / HONORMTN-AN00）
· 系统：Android 16，构建指纹 HONOR/MTN-AN00/HNMTN-Q:16/HONORMTN-AN00/10DLDLD170C00E157:user/release-keys
· 内核：偏移已在校准文件中硬编码，其他内核版本需重新校准 target.h 与 struct.h

在其他设备或内核上运行可能导致崩溃、变砖或不可预知的行为。请务必备份。

---

适配新内核/设备

本项目将内核相关的所有偏移与结构体布局集中到两个文件中，两者必须同时适配，缺一不可：

· src/targets/<PROJECT>/target.h：内核符号地址、全局变量偏移、常量地址等。
· src/targets/<PROJECT>/struct.h：目标内核特有结构体的字段布局（如 rb_node、rt_mutex_waiter、file_operations、pipe_buffer 等）。

适配过程主要依赖反汇编。若内核启用了 BTF（CONFIG_DEBUG_INFO_BTF=y），则可快速获取部分结构体信息。

1. 提取内核镜像与符号

· 从设备固件包或 boot.img 中解包出 vmlinux（未压缩内核镜像）。
· 若设备允许，也可从 /proc/kallsyms 获取符号地址（需 root 或 kptr_restrict=0）。
· 推荐工具：extract-vmlinux、unpackbootimg、magiskboot。

2. 使用 BTF 快速获取结构体布局（若可用）

如果 vmlinux 包含 BTF 信息，可使用 pahole 直接导出结构体：

```bash
# 查看结构体定义
pahole -C task_struct vmlinux
pahole -C cred vmlinux
pahole -C pipe_buffer vmlinux

# 导出所有结构体到文件，再手动摘录到 struct.h
pahole --headers vmlinux > structs_dump.h
```

根据输出更新 struct.h 中的对应结构体定义。

3. 使用反汇编获取符号地址与偏移

对于 target.h 中的符号地址，使用 readelf 与 objdump：

```bash
# 列出符号及其地址
readelf -sW vmlinux | grep -E 'ashmem_ioctl|configfs_read_iter|noop_llseek|init_task|selinux_enforcing'

# 反汇编指定范围
objdump -d vmlinux --start-address=0xffffffc008000000 --stop-address=0xffffffc009000000
```

计算符号相对于 _text 的偏移，填入 target.h 中的 *_OFF 宏。例如：

```c
#define ASHMEM_IOCTL_OFF 0x00477f84ULL
#define INIT_TASK_OFF    0x020bf6c0ULL
```

注意：KIMAGE_TEXT_BASE 是内核链接基址，ARM64 39 位 VA 通常为 0xffffffc008000000，具体以目标内核为准。

4. 手动推导结构体偏移

若没有 BTF，可结合内核源码与反汇编手动推导：

· 在反汇编中定位访问 task_struct、cred、file 等字段的指令，根据偏移推断字段位置。
· 参考对应内核版本的 include/linux/sched.h、include/linux/cred.h、include/linux/fs.h 等头文件。
· 使用 gdb 加载 vmlinux 辅助调试，或使用 crash 工具分析。

5. 验证

适配完成后：

```bash
make PROJECT=<你的项目名> info   # 检查配置
make PROJECT=<你的项目名>        # 编译
make PROJECT=<你的项目名> run    # 部署运行
```

若出现崩溃或读写失败，需重新核对 target.h 与 struct.h 中的偏移和结构体布局。

重要：两个文件必须匹配同一内核版本，否则利用链将无法正常工作。

---

注意事项

· 需要内核支持 perf_event_open 且未限制 perf_event_paranoid。
· 部分设备可能需要关闭 SELinux 或具有 CAP_SYS_ADMIN 等权限才能成功。
· 利用过程中会创建大量线程与 futex，可能短暂影响系统响应。
· 运行前建议关闭其他安全软件或内核防护模块。
· 本项目不提供任何在线更新或远程控制功能。

---

致谢

本项目建立在众多优秀开源工作之上，特此致谢：

· GhostLock (CVE-2026-43499) is by NebuSec — IonStack Part II — GhostLock, under Apache-2.0. The cves/ tree adds Pixel/aarch64 offsets and a KernelSU late-load daemon under the same terms.
· KernelSnitch：堆指针侧信道由 KernelSnitch 提供，作者 Lukas Maar 等，TU Graz，发表于 NDSS 2025。
· KernelSU：KernelSU 及其变体提供了本工具加载的模块与管理器模型；本项目与管理器无关，不捆绑任何 fork。
· 完整参考见 NOTICE。

---

再次提醒：仅限授权安全研究，遵守当地法律法规。
