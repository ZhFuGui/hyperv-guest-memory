# 按 CPU 架构构建

在 PowerShell 中加载已准备好的依赖环境，然后选择 Intel 或 AMD：

```powershell
. 'M:\hyperv-guest-memory-deps\env.ps1'
Set-Location 'G:\hyperv-guest-memory'
MSBuild.exe .\hyperv-guest-memory.slnx /m:3 /t:Build /p:Configuration=Release /p:Platform=x64 /p:HypervisorArchitecture=Intel /p:VcpkgManifestInstall=false /p:VcpkgApplocalDeps=false /nr:false
MSBuild.exe .\hyperv-guest-memory.slnx /m:3 /t:Build /p:Configuration=Release /p:Platform=x64 /p:HypervisorArchitecture=AMD /p:VcpkgManifestInstall=false /p:VcpkgApplocalDeps=false /nr:false
```

`HypervisorArchitecture` 默认是 `Intel`，仅接受 `Intel`、`AMD`。无需修改头文件。该参数选择 hyperv-attachment 的 CPU 实现；UEFI 和 usermode 也放入对应目录，形成完整的一套产物。

- 最终产物：`build\Intel\x64\Release\` 或 `build\AMD\x64\Release\`。
- 中间文件：`obj\<架构>\x64\Release\<项目名>\`，切换架构时各自保留。
- 编译 hyperv-attachment 成功后，自动将根目录的 `load-hyperv-guest-memory.bat` 复制到对应产物目录。只复制，不执行。

每套目录包含 `uefi-boot.efi`、`hyperv-attachment.dll`、`usermode.exe` 和加载脚本。按目标机器的 CPU 选择对应目录。

上面的命令复用已安装的依赖；若依赖清单变更，应先重新安装依赖。编译成功不等于已完成实际引导验证。

依赖位于仓库之外的 `M:\hyperv-guest-memory-deps`；`vendor\` 下的目录联接指向该根目录。环境脚本为 `M:\hyperv-guest-memory-deps\env.ps1`，vcpkg 安装目录及缓存也位于该根目录内。迁移依赖目录后，应同步更新联接和路径记录，并重新编译依赖及项目。

## 捕获路径的隔离验证

安装 Python 3 和上述 Visual Studio C++ 工具后，在仓库根目录运行：

```powershell
python .\hyperv-attachment\tests\isolated\run.py
```

默认验证 AMD 和 Intel，也可用 `--architecture AMD` 或 `--architecture Intel` 单独运行。脚本将待测生产源码逐字节复制到 `build\isolated-tests\<架构>\`，使用真实页表拆分、地址转换、捕获队列、陷阱状态机及退出分发代码，替换硬件访问和宿主环境。结果保存在该目录的 `results.txt`。它不会加载 payload、执行真实超调用、修改引导或连接远程主机。

覆盖队列并发及计数守恒、写回失败、消费者互斥、满环重试、同页多 CPU 捕获、设陷与清理交错、哈希碰撞、槽位回收、大页拆分和分配失败、原权限和 A/D 位、Intel MBEC、刷新代次及退出转发。隔离通过和构建成功不能代替实际引导及真实负载验证；Hyper-V 私有挂接点的调用约定仍须随系统版本核对。

捕获命令的约定：

- `trap` 仅撤销执行权限，保留原读写权限；已捕获页保持一次性语义，缓冲满导致丢弃的页可在下一次扫描时重试。要开始全新一轮采集，先 `trapclear`。
- `trap` 每 64 页及批次结束时通知其他 CPU 刷新。`trapstats` 显示 `pending_cpus`；为零表示该刷新代次已被所有 CPU 处理，设置请求被接受不代表所有 CPU 已立即生效。
- 单次 `capture_flush` 超调用最多取出 16 条记录；本项目的 `capflush` 命令会在用户态分批取到请求上限。旧的独立客户端也应按实际返回数量循环调用。
- `trapclear` 返回剩余注册数；`UINT64_MAX` 表示控制操作忙，应重试。捕获与 shadow hook/隐藏操作互斥，尚未完成刷新或读者回收时后者也可能返回失败。
- 捕获最多支持 32 个 CPU，超限会拒绝设陷，不会共用 CPU 0 的环。记录仍为 4144 字节，`flags` 记录生产 CPU 编号。4 KiB 拷贝不是针对其他 CPU 并发写入的原子快照。
