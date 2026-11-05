# 按 CPU 架构构建

在 PowerShell 中加载已准备好的依赖环境，然后选择 Intel 或 AMD：

```powershell
. 'M:\hyperv-memory-deps\env.ps1'
Set-Location 'G:\hyperv-memory'
MSBuild.exe .\hyperv-memory.slnx /m:3 /t:Build /p:Configuration=Release /p:Platform=x64 /p:HypervisorArchitecture=Intel /p:VcpkgManifestInstall=false /p:VcpkgApplocalDeps=false /nr:false
MSBuild.exe .\hyperv-memory.slnx /m:3 /t:Build /p:Configuration=Release /p:Platform=x64 /p:HypervisorArchitecture=AMD /p:VcpkgManifestInstall=false /p:VcpkgApplocalDeps=false /nr:false
```

`HypervisorArchitecture` 默认是 `Intel`，仅接受 `Intel`、`AMD`。无需修改头文件。该参数选择 hyperv-attachment 的 CPU 实现；UEFI 和 usermode 也放入对应目录，形成完整的一套产物。

- 最终产物：`build\Intel\x64\Release\` 或 `build\AMD\x64\Release\`。
- 中间文件：`obj\<架构>\x64\Release\<项目名>\`，切换架构时各自保留。
- 编译 hyperv-attachment 成功后，自动将根目录的 `load-hyperv-memory.bat` 复制到对应产物目录。只复制，不执行。

每套目录包含 `uefi-boot.efi`、`hyperv-attachment.dll`、`usermode.exe` 和加载脚本。按目标机器的 CPU 选择对应目录。

上面的命令复用已安装的依赖；若依赖清单变更，应先重新安装依赖。编译成功不等于已完成实际引导验证。
