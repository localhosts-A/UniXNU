# 内核预链接

预链接内核把源码构建的内核和原始 Apple kernelcache 合并。源码内核提供 UniXNU 修改的代码，原始 kernelcache 提供 Apple kext 和平台服务，源码树把这些留给 kernelcache。

## 输入

| 输入 | 来源 |
| --- | --- |
| 原生内核 | `out/<board>/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf` |
| 原始 kernelcache | IPSW `KernelCache`，或 `--kernelcache FILE` |
| 组件列表 | `[kernel.prelink] roots` |
| 原生内核提供的组件身份 | `[kernel.prelink] source_interfaces` |
| slide | `[kernel.prelink] slide` |
| 早期安全初始化器 | `[kernel.prelink] early_security_init` |

kernelcache 是 `KernelCache` IM4P 的 LZFSE 负载。选出 arm64 切片，并按 `kernelcache_sha256` 校验。

## 预链接步骤

`kits/NativePrelink/emit_prelink.py`：

1. 解析原生和原始 Mach-O。
2. 读取原始 kernelcache 的 `__PRELINK_INFO`。
3. 选择 `roots` 中的组件及其导入的内核接口。
   若原生内核已包含某个组件实现，板级文件可声明 `source_interfaces`。
   选定的预链接工具必须保留 bundle 版本、依赖和 IOKit personality，
   作为无代码元数据处理，并移除重复可执行代码与 kmod 记录。默认
   NativePrelink emitter 本身不会自动执行这项板级转换。
4. 把选中的组件段复制到原生内核的空合并段。
5. 重绑定引用选中组件的指针。
6. 对所有选中地址应用 slide。
7. 重写模块头和 kmod 条目。
8. 构建稀疏预链接索引。
9. 写出 `kernel.macho`。

输出保留原始组件代码不变。原生内核提供平台、CPU、存储和 defconfig 选中的设备驱动。

## 合并段

原始 kernelcache 把 kext 段放在合并范围：

| 原始段 | 原生段 |
| --- | --- |
| `__PRELINK_TEXT` | `__TEXT` |
| `__PLK_TEXT_EXEC` | `__TEXT_EXEC` |
| `__PLK_DATA_CONST` | `__DATA_CONST` |
| `__PRELINK_DATA` | `__DATA` |

预链接把每个组件段复制到对应范围。范围保留原始虚拟地址，slide 后原始代码中的指针仍然有效。

## 指针重绑定

原生内核从原始组件导入符号。预链接把每个导入解析为组件导出，并在导入位置写入 slide 后的地址。板级 source-interface 工具必须把指向已省略可执行代码的导入唯一绑定到原生内核导出；找不到或存在歧义时立即拒绝。

计划记录每个绑定：

```json
{
  "bindings": [
    { "new_site": "0xfffffff007123456", "new_target": "0xfffffff004abcdef" }
  ]
}
```

报告写在输出旁边，包含组件列表、指令清单和模块入口。

## 早期安全初始化

`early_security_init` 选择一个原始安全扩展及其可执行段中的地址。预链接对地址应用 slide，并记录给内核的早期安全路径。组件在预链接元数据中必须带 `AppleSecurityExtension = true`。

## kernelcache 固定

板级文件可以固定 kernelcache：

```toml
[kernel.prelink]
kernelcache_sha256 = "..."
```

预链接拒绝不同的 kernelcache。这样原生内核与它验证过的原始组件 ABI 绑定。

## 稀疏与容量

预链接输出稀疏索引。补丁 `native-4k-bootstrap-capacity` 增加原生 4 KiB 映射的 bootstrap 页表容量。启动进度补丁在预链接路径周围加入检查点输出。

## 报告

`emit_prelink.py` 在输出旁边写 JSON 报告：

| 字段 | 含义 |
| --- | --- |
| `components` | 包含的原始 bundle identifier |
| `source_interfaces` | 由原生内核实现的原始组件身份 |
| `codeless_components` | 保留 bundle 元数据和 personalities、但不含重复可执行代码的组件 |
| `kernel_interfaces` | 导出内核接口的组件 |
| `bindings` | 导入位置和 slide 后的目标 |
| `instruction_inventory` | PC 相对编码和内联数据 |
| `module_entrypoints` | slide 后的模块加载地址 |
| `early_security_initializers` | 选中的早期初始化器地址 |

## 下一步

- [构建内核](/zh/guide/build-kernel)
- [4 KiB 页表](/zh/xnu/four-k-page-tables)
- [完整性与启动策略](/zh/concepts/security)
