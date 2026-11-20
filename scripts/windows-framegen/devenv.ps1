# Loads the VS 2026 x64 developer environment plus the variables CI uses.
# Usage: . .\tools\devenv.ps1   (dot-source)
$ErrorActionPreference = 'Stop'
$env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\Installer;$env:PATH"   # vswhere
$root = Split-Path -Parent $PSScriptRoot           # ...\chakai_60fps
$vs   = 'C:\Program Files\Microsoft Visual Studio\18\Insiders'
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

$env:CC  = 'clang-cl.exe'
$env:CXX = 'clang-cl.exe'
$env:VULKAN_SDK = 'C:\VulkanSDK\'
$env:CHIAKI_SRC = "$root\chiaki-ng"
$env:QT_PREFIX  = "$root\tools\Qt\6.12.1"
$env:DEPS       = "$root\chiaki-ng\deps"
$env:VCPKG_INSTALLED_DIR = "$root\chiaki-ng\build\vcpkg_installed"
$env:PATH = "$root\tools\venv\Scripts;$env:QT_PREFIX\bin;C:\VulkanSDK\Bin;$env:PATH"
# PS 5.1 turns native stderr into errors; don't let warnings abort builds.
$ErrorActionPreference = 'Continue'
