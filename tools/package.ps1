param(
  [string]$OutDir = "dist",
  [string]$AndroidAbi = "arm64-v8a",
  [int]$AndroidApi = 31,
  [string]$NdkPath = "",
  [string]$NinjaPath = "",
  [switch]$BuildHost = $true,
  [switch]$BuildAndroidSo = $true,
  [switch]$Zip = $true
)

$ErrorActionPreference = "Stop"

function Resolve-PathSafe([string]$p) {
  if ([string]::IsNullOrWhiteSpace($p)) { return "" }
  try { return (Resolve-Path -LiteralPath $p).Path } catch { return "" }
}

function Ensure-Dir([string]$p) {
  if (-not (Test-Path -LiteralPath $p)) {
    New-Item -ItemType Directory -Path $p | Out-Null
  }
}

function Find-LatestDir([string]$root) {
  if (-not (Test-Path -LiteralPath $root)) { return "" }
  $d = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
  if ($null -eq $d) { return "" }
  return $d.FullName
}

function Find-So([string]$buildDir) {
  if (-not (Test-Path -LiteralPath $buildDir)) { return "" }
  $f = Get-ChildItem -LiteralPath $buildDir -Recurse -File -Filter "libcarserial.so" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if ($null -eq $f) { return "" }
  return $f.FullName
}

function Find-Ninja([string]$hint) {
  $p = Resolve-PathSafe $hint
  if ($p -and (Test-Path -LiteralPath $p)) { return $p }

  $cmd = Get-Command ninja -ErrorAction SilentlyContinue
  if ($cmd -and $cmd.Source) { return $cmd.Source }

  $candidates = @(
    (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links\ninja.exe"),
    "C:\Program Files\ninja\ninja.exe",
    "C:\Program Files (x86)\ninja\ninja.exe",
    "C:\Program Files\Ninja\ninja.exe",
    "C:\Program Files (x86)\Ninja\ninja.exe"
  )
  foreach ($c in $candidates) {
    if (Test-Path -LiteralPath $c) { return $c }
  }

  $pkgRoot = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
  if (Test-Path -LiteralPath $pkgRoot) {
    $hit = Get-ChildItem -LiteralPath $pkgRoot -Directory -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -like "Ninja-build.Ninja*" } |
      Sort-Object LastWriteTime -Descending |
      Select-Object -First 3
    foreach ($d in $hit) {
      $exe = Get-ChildItem -LiteralPath $d.FullName -Recurse -File -Filter "ninja.exe" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
      if ($exe -and (Test-Path -LiteralPath $exe.FullName)) { return $exe.FullName }
    }
  }
  return ""
}

function Run([string]$cmd) {
  Write-Host ">> $cmd"
  Invoke-Expression $cmd
}

$root = (Get-Location).Path
Write-Host "Project: $root"

$out = Join-Path $root $OutDir
Ensure-Dir $out

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$bundleDir = Join-Path $out ("bundle_" + $stamp)
Ensure-Dir $bundleDir

if ($BuildHost) {
  $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
  if (-not $cmake) {
    $cmake = "C:\Program Files\CMake\bin\cmake.exe"
  }
  if (-not (Test-Path -LiteralPath $cmake)) {
    throw "CMake not found. Install CMake first."
  }

  $hostBuild = Join-Path $root "build\native-host"
  Ensure-Dir $hostBuild

  Run "& `"$cmake`" -S carserial/native -B `"$hostBuild`" -G `"Visual Studio 17 2022`" -A x64"
  Run "& `"$cmake`" --build `"$hostBuild`" --config Release"

  $exe = Join-Path $hostBuild "Release\carserial_host_tool.exe"
  if (-not (Test-Path -LiteralPath $exe)) {
    $exe = (Get-ChildItem -LiteralPath $hostBuild -Recurse -File -Filter "carserial_host_tool.exe" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
  }
  if (-not $exe -or -not (Test-Path -LiteralPath $exe)) {
    throw "carserial_host_tool.exe not found after build."
  }
  $dst = Join-Path $bundleDir "host\windows-x64"
  Ensure-Dir $dst
  Copy-Item -LiteralPath $exe -Destination (Join-Path $dst "carserial_host_tool.exe") -Force
}

if ($BuildAndroidSo) {
  if ([string]::IsNullOrWhiteSpace($NdkPath)) {
    $NdkPath = $env:ANDROID_NDK_HOME
  }
  if ([string]::IsNullOrWhiteSpace($NdkPath)) {
    $guess = Join-Path $env:LOCALAPPDATA "Android\Sdk\ndk"
    $NdkPath = Find-LatestDir $guess
  }
  $NdkPath = Resolve-PathSafe $NdkPath
  if ([string]::IsNullOrWhiteSpace($NdkPath)) {
    throw "NDK not found. Set -NdkPath or ANDROID_NDK_HOME."
  }

  $toolchain = Join-Path $NdkPath "build\cmake\android.toolchain.cmake"
  if (-not (Test-Path -LiteralPath $toolchain)) {
    throw "NDK toolchain not found: $toolchain"
  }

  $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
  if (-not $cmake) { $cmake = "C:\Program Files\CMake\bin\cmake.exe" }
  if (-not (Test-Path -LiteralPath $cmake)) { throw "CMake not found." }

  $ninja = Find-Ninja $NinjaPath
  if (-not $ninja) {
    Write-Host "Ninja not found. Recommend: winget install Ninja-build.Ninja -e"
    throw "Ninja is required for Android CMake build on Windows. If it is installed but not in PATH, pass -NinjaPath <path-to-ninja.exe>."
  }
  $ninja = Resolve-PathSafe $ninja
  if ($ninja -and $ninja.Length -gt 0) {
    $needCopy = $false
    if ($ninja.Length -gt 140) { $needCopy = $true }
    if ($ninja -like "*\\Microsoft\\WinGet\\Packages\\Ninja-build.Ninja_*\\ninja.exe") { $needCopy = $true }
    if ($needCopy) {
      $tmpNinja = Join-Path $env:TEMP ("ninja_" + [guid]::NewGuid().ToString("N") + ".exe")
      Copy-Item -LiteralPath $ninja -Destination $tmpNinja -Force
      $ninja = $tmpNinja
    }
  }

  $androidBuild = Join-Path $root ("build\android-" + $AndroidAbi)
  Ensure-Dir $androidBuild

  Run "& `"$cmake`" -S carserial/native -B `"$androidBuild`" -G `"Ninja`" -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DANDROID_ABI=$AndroidAbi -DANDROID_PLATFORM=android-$AndroidApi -DCMAKE_TOOLCHAIN_FILE=`"$toolchain`""
  Run "& `"$cmake`" --build `"$androidBuild`""

  $so = Find-So $androidBuild
  if ([string]::IsNullOrWhiteSpace($so)) {
    throw "libcarserial.so not found under $androidBuild"
  }
  $dst = Join-Path $bundleDir ("android\jniLibs\" + $AndroidAbi)
  Ensure-Dir $dst
  Copy-Item -LiteralPath $so -Destination (Join-Path $dst "libcarserial.so") -Force
}

Ensure-Dir (Join-Path $bundleDir "src")
Copy-Item -LiteralPath (Join-Path $root "carserial") -Destination (Join-Path $bundleDir "src\carserial") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root "app") -Destination (Join-Path $bundleDir "src\app") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root "tools") -Destination (Join-Path $bundleDir "src\tools") -Recurse -Force

if (Test-Path -LiteralPath (Join-Path $root "doc")) {
  Copy-Item -LiteralPath (Join-Path $root "doc") -Destination (Join-Path $bundleDir "doc") -Recurse -Force
}

if ($Zip) {
  $zipPath = Join-Path $out ("bundle_" + $stamp + ".zip")
  if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
  Compress-Archive -LiteralPath $bundleDir -DestinationPath $zipPath
  Write-Host "ZIP: $zipPath"
}

Write-Host "Bundle: $bundleDir"
