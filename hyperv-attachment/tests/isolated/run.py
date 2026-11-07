"""Compile byte-for-byte production sources against an isolated hardware model.

Requires Python 3 and the VS C++ tools used by BUILDING.md. Never loads a driver,
executes a hypercall, changes firmware, or contacts the remote test machine.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import xml.sax.saxutils as xml

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SOURCES = ["capture/capture.cpp", "slat/hook/trap.cpp", "slat/hook/cache.cpp",
           "slat/cr3/split.cpp", "slat/violation/violation.cpp", "vmexit/vmexit.cpp",
           "vmexit/first/first.cpp"]
HEADERS = ["capture/capture.hpp", "slat/hook/trap.hpp", "slat/hook/cache.hpp",
           "slat/cr3/split.hpp", "slat/violation/violation.hpp", "vmexit/vmexit.hpp",
           "vmexit/first/first.hpp", "arch/amd.hpp", "arch/amd_def.hpp", "arch/intel.hpp",
           "arch/arch.hpp", "arch/defs.hpp", "arch/exit_reason.hpp", "mem/guest.hpp", "mem/mem.hpp"]
STUBS = ["arch/current.hpp", "slat/cr3/cr3.hpp", "slat/hook/info.hpp",
         "slat/hook/page_split.hpp", "slat/hook/hook.hpp", "slat/slat.hpp", "mem/map.hpp",
         "mem/alloc/page.hpp", "vcpu/vcpu.hpp", "intr/nmi.hpp", "vmexit/hypercall/hypercall.hpp"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture", choices=["AMD", "Intel", "all"], default="all")
    args = parser.parse_args()
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = Path(subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                      "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                      "-property", "installationPath"], text=True).strip())
    msbuild = vs / "MSBuild/Current/Bin/MSBuild.exe"
    for architecture in (["AMD", "Intel"] if args.architecture == "all" else [args.architecture]):
        work = ROOT / "build/isolated-tests" / architecture
        work.mkdir(parents=True, exist_ok=True)
        for name in SOURCES + HEADERS:
            target = work / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / "hyperv-attachment/src" / name, target)
            assert target.read_bytes() == (ROOT / "hyperv-attachment/src" / name).read_bytes()
        for name in STUBS:
            target = work / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('#pragma once\n#include "mock.hpp"\n', encoding="utf-8")
        for name in ["mock.hpp", "mocks.cpp", "main.cpp", "crt.hpp"]:
            shutil.copy2(HERE / name, work / name)
        includes = [work, ROOT / "shared", ROOT / "vendor/cstd/src", ROOT / "vendor/ia32-doc/out"]
        items = "".join(f'<ClCompile Include="{xml.escape(str(work / s))}" />' for s in SOURCES + ["mocks.cpp", "main.cpp"])
        project = f'''<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<ItemGroup Label="ProjectConfigurations"><ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration></ItemGroup>
<PropertyGroup Label="Globals"><Keyword>Win32Proj</Keyword><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
<PropertyGroup Label="Configuration"><ConfigurationType>Application</ConfigurationType><PlatformToolset>v145</PlatformToolset><UseDebugLibraries>false</UseDebugLibraries></PropertyGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
<PropertyGroup><OutDir>{xml.escape(str(work))}\\</OutDir><IntDir>{xml.escape(str(work / 'obj'))}\\</IntDir><TargetName>isolated-tests</TargetName></PropertyGroup>
<ItemDefinitionGroup><ClCompile><LanguageStandard>stdcpp20</LanguageStandard><Optimization>MaxSpeed</Optimization><ExceptionHandling>Sync</ExceptionHandling><AdditionalIncludeDirectories>{';'.join(xml.escape(str(p)) for p in includes)}</AdditionalIncludeDirectories><PreprocessorDefinitions>MOCK_{architecture.upper()};NOMINMAX;_CRT_SECURE_NO_WARNINGS</PreprocessorDefinitions><WarningLevel>Level4</WarningLevel></ClCompile><Link><SubSystem>Console</SubSystem></Link></ItemDefinitionGroup>
<ItemGroup>{items}</ItemGroup><Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" /></Project>'''
        path = work / "isolated-tests.vcxproj"
        path.write_text(project, encoding="utf-8")
        subprocess.run([str(msbuild), str(path), "/p:Configuration=Release", "/p:Platform=x64",
                        "/p:VcpkgManifestInstall=false", "/p:VcpkgApplocalDeps=false", "/nr:false", "/v:minimal"], check=True)
        result = subprocess.run([str(work / "isolated-tests.exe")], capture_output=True, text=True, timeout=60)
        (work / "results.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
        print(architecture + ":\n" + result.stdout + result.stderr, flush=True)
        result.check_returncode()


if __name__ == "__main__":
    main()
