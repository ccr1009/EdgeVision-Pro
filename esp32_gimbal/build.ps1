# 在 ESP-IDF 环境下构建 esp32_gimbal
# 用法: pwsh -File build.ps1 [-Clean] [-Flash] [-Monitor] [-Port COM5]
#
# 注意：本机 IDF v6.0.1 自带的 export.ps1 会去找 python_env\idf6.0_py3.12_env，
# 但实际安装的是 idf6.0_py3.13_env，导致载入失败。
# 这里不修改 IDF 安装本身，而是直接设置 IDF_PYTHON_ENV_PATH 后调用 idf.py。

param(
    [switch]$Clean,
    [switch]$Flash,
    [switch]$Monitor,
    [string]$Port = "COM5"
)

$ErrorActionPreference = "Stop"

$idfPath  = "C:\Users\15354\esp\esp-idf-v6.0.1"
$toolsPath = "C:\Users\15354\.espressif"
$pyEnv    = Join-Path $toolsPath "python_env\idf6.0_py3.13_env"
$projDir  = $PSScriptRoot

$env:IDF_PATH = $idfPath
$env:IDF_TOOLS_PATH = $toolsPath
$env:IDF_PYTHON_ENV_PATH = $pyEnv
$env:IDF_TARGET = "esp32s3"
# export.ps1 会设置这个；我们绕过了它，必须手动补——
# 缺了它 idf_component_manager 解析版本时拿到 None 直接抛 TypeError。
$env:ESP_IDF_VERSION = "6.0.1"
$env:ESP_ROM_ELF_DIR = "$toolsPath\tools\esp-rom-elfs\20250925"

$py = Join-Path $pyEnv "Scripts\python.exe"
if (-not (Test-Path $py)) {
    Write-Host "找不到 IDF python: $py" -ForegroundColor Red
    exit 1
}

# 把工具链加进 PATH。
# .espressif\tools 下的布局并不统一，实测三种：
#   ninja\1.12.1\ninja.exe                          → 版本目录直接放 exe
#   cmake\4.0.3\bin\cmake.exe                       → 版本目录下还有 bin
#   xtensa-esp-elf\esp-15.2.0_xxx\xtensa-esp-elf\bin\  → 多套了一层同名的工具链根目录
# 所以不能硬编码层级，直接递归找含 .exe 的 bin 目录（限深 4 层）。
$found = New-Object System.Collections.ArrayList
foreach ($tool in (Get-ChildItem (Join-Path $toolsPath "tools") -Directory -ErrorAction SilentlyContinue)) {
    # 含可执行文件的 bin 目录
    Get-ChildItem $tool.FullName -Recurse -Depth 3 -Directory -Filter "bin" -ErrorAction SilentlyContinue |
        ForEach-Object {
            if (-not $found.Contains($_.FullName)) { [void]$found.Add($_.FullName) }
        }
    # ninja 这类 exe 直接放在版本目录里
    Get-ChildItem $tool.FullName -Recurse -Depth 3 -Filter "*.exe" -ErrorAction SilentlyContinue |
        ForEach-Object {
            $d = $_.DirectoryName
            if (-not $found.Contains($d)) { [void]$found.Add($d) }
        }
}
foreach ($d in $found) { $env:PATH = "$d;$env:PATH" }
Write-Host "PATH 追加了 $($found.Count) 个工具目录"

# 硬校验：编译器必须能找到，否则后面 cmake 报错更难懂
$gcc = Get-Command "xtensa-esp32s3-elf-gcc" -ErrorAction SilentlyContinue
if (-not $gcc) {
    Write-Host "找不到 xtensa-esp32s3-elf-gcc" -ForegroundColor Red
    exit 1
}
Write-Host "编译器: $($gcc.Source)"

Write-Host "IDF_PATH          = $env:IDF_PATH"
Write-Host "IDF_PYTHON_ENV    = $env:IDF_PYTHON_ENV_PATH"
Write-Host "IDF_TARGET        = $env:IDF_TARGET"
$idfVer = & $py -c "import sys; print(sys.version.split()[0])"
Write-Host "python            = $idfVer"

Set-Location $projDir

if ($Clean) {
    Write-Host "=== 清理 ===" -ForegroundColor Cyan
    if (Test-Path "build") { Remove-Item -Recurse -Force "build" }
    if (Test-Path "sdkconfig") { Remove-Item -Force "sdkconfig" }
}

Write-Host "=== 构建 ===" -ForegroundColor Cyan
$idfPy = Join-Path $idfPath "tools\idf.py"
& $py $idfPy -DIDF_TARGET=esp32s3 build
if ($LASTEXITCODE -ne 0) {
    Write-Host "构建失败 exit=$LASTEXITCODE" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "=== 构建成功 ===" -ForegroundColor Green

$binPath = Join-Path $projDir "build\esp32_gimbal.bin"
if (Test-Path $binPath) {
    $sz = (Get-Item $binPath).Length
    Write-Host ("固件: {0} ({1:N0} bytes)" -f $binPath, $sz)
}

if ($Flash) {
    Write-Host "=== 烧录到 $Port ===" -ForegroundColor Cyan
    & $py $idfPy -p $Port flash
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Monitor) {
    Write-Host "=== 串口监视（Ctrl+] 退出）===" -ForegroundColor Cyan
    & $py $idfPy -p $Port monitor
}
