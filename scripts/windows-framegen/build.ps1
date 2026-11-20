# Local reproduction of .github/workflows/build-windows-x86.yml (VC path).
# Usage: powershell -File tools\build.ps1 <step>     step = placebo | configure | build | deploy | all
# Logs: tools\logs_<step>.txt
param([Parameter(Mandatory)][ValidateSet('placebo','configure','build','deploy','all')][string]$Step)

. "$PSScriptRoot\devenv.ps1"
Remove-Item Env:VCPKG_ROOT -ErrorAction SilentlyContinue
$src = $env:CHIAKI_SRC
$vi  = "$env:VCPKG_INSTALLED_DIR\x64-windows"
$logdir = $PSScriptRoot

function Invoke-Logged([string]$name, [string]$cmdline) {
    $log = "$logdir\logs_$name.txt"
    cmd /c "$cmdline > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Write-Host "[$name] FAILED (exit $LASTEXITCODE), log: $log"; exit $LASTEXITCODE }
    Write-Host "[$name] OK"
}

function Step-Placebo {
    Set-Location "$src\libplacebo"
    if (Test-Path build) { Remove-Item -Recurse -Force build }
    $pkg = "['$vi\lib\pkgconfig','$vi\share\pkgconfig','$env:DEPS\lib\pkgconfig']"
    $cm  = "['$vi','C:\VulkanSDK','$env:DEPS']"
    Invoke-Logged 'placebo_setup' ("meson setup --prefix `"$env:DEPS`" --native-file ../meson.ini " +
        "`"--pkg-config-path=$pkg`" `"--cmake-prefix-path=$cm`" " +
        "`"-Dc_args=/I C:\VulkanSDK\Include`" `"-Dcpp_args=/I C:\VulkanSDK\Include`" " +
        "`"-Dc_link_args=/LIBPATH:C:\VulkanSDK\Lib`" `"-Dcpp_link_args=/LIBPATH:C:\VulkanSDK\Lib`" " +
        "-Ddemos=false ./build")
    Invoke-Logged 'placebo_build' 'ninja -C ./build'
    Invoke-Logged 'placebo_install' 'ninja -C ./build install'
}

function Step-Configure {
    Set-Location $src
    $py = (Resolve-Path "$PSScriptRoot\venv\Scripts\python.exe").Path
    Invoke-Logged 'configure' ("cmake -S . -B build -G Ninja " +
        "-DCMAKE_TOOLCHAIN_FILE:STRING=vcpkg/scripts/buildsystems/vcpkg.cmake " +
        "-DVCPKG_INSTALLED_DIR=`"$env:VCPKG_INSTALLED_DIR`" -DVCPKG_MANIFEST_INSTALL=OFF " +
        "-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DCMAKE_BUILD_TYPE=Release " +
        "-DCHIAKI_ENABLE_CLI=OFF -DCHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER=ON -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF " +
        "-DPYTHON_EXECUTABLE=`"$py`" " +
        "`"-DCMAKE_PREFIX_PATH=$env:DEPS;C:\VulkanSDK;$env:QT_PREFIX`"")
}

function Step-Build {
    Set-Location $src
    Invoke-Logged 'build' 'cmake --build build --config Release --target chiaki'
}

function Step-Deploy {
    Set-Location $src
    $out = "$src\chiaki-ng-Win"
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    New-Item -ItemType Directory $out | Out-Null
    Copy-Item build\gui\chiaki.exe, build\third-party\cpp-steam-tools\cpp-steam-tools.dll $out
    Copy-Item scripts\qtwebengine_import.qml gui\src\qml\
    # CI uses a fixed DLL list that is stale for newer vcpkg (e.g. lcms2-2.dll, no hidapi/fftw3 when
    # SteamDeck support is off). Resolve the real import closure with dumpbin instead.
    # Search order matters: deps\bin first so SDL2.dll is sdl2-compat (as in CI), not vcpkg's SDL2.
    $search = @("$env:DEPS\bin", "$vi\bin", 'C:\VulkanSDK\Bin')
    Copy-Item "$env:DEPS\bin\SDL3.dll" $out        # loaded at runtime by sdl2-compat, not an import
    $done = @{}
    $queue = [System.Collections.Queue]::new()
    Get-ChildItem $out -Include *.exe,*.dll -Recurse | ForEach-Object { $queue.Enqueue($_.FullName) }
    while ($queue.Count -gt 0) {
        $f = $queue.Dequeue()
        if ($done[$f]) { continue }; $done[$f] = $true
        $deps = (dumpbin /nologo /dependents $f) | Where-Object { $_ -match '^\s+(\S+\.dll)\s*$' } | ForEach-Object { $Matches[1] }
        foreach ($d in $deps) {
            if ((Test-Path "$out\$d") -or $d -ieq 'vulkan-1.dll') { continue }   # Vulkan loader comes from the GPU driver
            foreach ($dir in $search) {
                if (Test-Path "$dir\$d") { Copy-Item "$dir\$d" $out; $queue.Enqueue("$out\$d"); break }
            }
        }
    }
    Invoke-Logged 'deploy' "windeployqt.exe --no-translations --qmldir=gui\src\qml --release `"$out\chiaki.exe`""
    # qtwebengine_import.qml is only needed by windeployqt scanning; keep the source tree clean.
    Remove-Item gui\src\qml\qtwebengine_import.qml
}

switch ($Step) {
    'placebo'   { Step-Placebo }
    'configure' { Step-Configure }
    'build'     { Step-Build }
    'deploy'    { Step-Deploy }
    'all'       { Step-Placebo; Step-Configure; Step-Build; Step-Deploy }
}
