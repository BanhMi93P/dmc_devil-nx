# PowerShell host build environment for dmc_devil-nx (never writes to C:).
$ErrorActionPreference = "Stop"
$script:WorkRoot = "G:\!Project\!port\DmC - Devil May Cry"
$script:TempDir = Join-Path $script:WorkRoot "temp"
New-Item -ItemType Directory -Force -Path $script:TempDir | Out-Null
$env:TEMP = $script:TempDir
$env:TMP = $script:TempDir

$vsDll = "D:\Visual Studio\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
if (-not (Get-Command Enter-VsDevShell -ErrorAction SilentlyContinue)) {
    Import-Module $vsDll
}
Enter-VsDevShell -VsInstallPath "D:\Visual Studio" -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

$cmakeBin = "D:\Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninjaBin = "D:\Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$llvmBin = "C:\Program Files\LLVM\bin"
$pyBin = "C:\Users\Admin\AppData\Local\Programs\Python\Python311"
$env:PATH = "$pyBin;$cmakeBin;$ninjaBin;$llvmBin;$env:PATH"

$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$env:REXGLUE_DUMP_IMAGE = Join-Path $script:RepoRoot "out\renut_image.bin"

$script:RexCmakeCompilers = @(
    "-DCMAKE_C_COMPILER=C:/Program Files/LLVM/bin/clang.exe",
    "-DCMAKE_CXX_COMPILER=C:/Program Files/LLVM/bin/clang++.exe",
    "-DCMAKE_C_COMPILER_TARGET=x86_64-pc-windows-msvc",
    "-DCMAKE_CXX_COMPILER_TARGET=x86_64-pc-windows-msvc",
    "-DCMAKE_LINKER_TYPE=LLD",
    "-DCMAKE_RC_COMPILER=C:/Program Files/LLVM/bin/llvm-rc.exe",
    "-DCMAKE_AR=C:/Program Files/LLVM/bin/llvm-ar.exe",
    "-DCMAKE_C_FLAGS=-march=x86-64-v2",
    "-DCMAKE_CXX_FLAGS=-march=x86-64-v2",
    "-DPYTHON_EXECUTABLE=C:/Users/Admin/AppData/Local/Programs/Python/Python311/python.exe",
    "-DPython3_EXECUTABLE=C:/Users/Admin/AppData/Local/Programs/Python/Python311/python.exe"
)
