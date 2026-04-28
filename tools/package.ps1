param(
  [string]$OutDir = "dist",
  [string]$AndroidAbi = "arm64-v8a",
  [int]$AndroidApi = 31,
  [string]$NdkPath = "",
  [string]$NinjaPath = "",
  [string]$BuildHost = "1",
  [string]$BuildAndroidSo = "1",
  [string]$Zip = "1"
)

$ErrorActionPreference = "Stop"

function Parse-Bool([object]$v, [bool]$defaultValue) {
  if ($null -eq $v) { return $defaultValue }
  $s = "$v"
  if ([string]::IsNullOrWhiteSpace($s)) { return $defaultValue }
  $s = $s.Trim()
  if ($s.StartsWith('$')) { $s = $s.Substring(1) }
  $s = $s.ToLowerInvariant()
  if ($s -eq "1" -or $s -eq "true" -or $s -eq "yes" -or $s -eq "y" -or $s -eq "on") { return $true }
  if ($s -eq "0" -or $s -eq "false" -or $s -eq "no" -or $s -eq "n" -or $s -eq "off") { return $false }
  return $defaultValue
}

$BuildHostFlag = Parse-Bool $BuildHost $true
$BuildAndroidSoFlag = Parse-Bool $BuildAndroidSo $true
$ZipFlag = Parse-Bool $Zip $true

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

function Find-PreferredNdk([string]$ndkRoot) {
  if (-not (Test-Path -LiteralPath $ndkRoot)) { return "" }
  $p = Join-Path $ndkRoot "26.1.10909125"
  if (Test-Path -LiteralPath $p) { return $p }
  return Find-LatestDir $ndkRoot
}

function Find-AndroidSdk() {
  $lp = Join-Path (Get-Location).Path "local.properties"
  if (Test-Path -LiteralPath $lp) {
    $lines = Get-Content -LiteralPath $lp -ErrorAction SilentlyContinue
    foreach ($l in $lines) {
      if ($l -like "sdk.dir=*") {
        $v = $l.Substring(8)
        $v = $v -replace "\\\\", "\"
        if (Test-Path -LiteralPath $v) { return $v }
      }
    }
  }
  $guess = Join-Path $env:LOCALAPPDATA "Android\Sdk"
  if (Test-Path -LiteralPath $guess) { return $guess }
  return ""
}

function Find-SdkCMake([string]$sdkDir) {
  if ([string]::IsNullOrWhiteSpace($sdkDir)) { return "" }
  $cmakeRoot = Join-Path $sdkDir "cmake"
  $latest = Find-LatestDir $cmakeRoot
  if ([string]::IsNullOrWhiteSpace($latest)) { return "" }
  $exe = Join-Path $latest "bin\cmake.exe"
  if (Test-Path -LiteralPath $exe) { return $exe }
  return ""
}

function Find-So([string]$buildDir) {
  if (-not (Test-Path -LiteralPath $buildDir)) { return "" }
  $f = Get-ChildItem -LiteralPath $buildDir -Recurse -File -Filter "libcarserial.so" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if ($null -eq $f) { return "" }
  return $f.FullName
}

function Find-AndroidTool([string]$buildDir) {
  if (-not (Test-Path -LiteralPath $buildDir)) { return "" }
  $f = Get-ChildItem -LiteralPath $buildDir -Recurse -File -Filter "carserial_host_tool" -ErrorAction SilentlyContinue |
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

if ($BuildHostFlag) {
  $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
  if (-not $cmake) {
    $cmake = "C:\Program Files\CMake\bin\cmake.exe"
  }
  if (-not (Test-Path -LiteralPath $cmake)) {
    $sdk = Find-AndroidSdk
    $cmake = Find-SdkCMake $sdk
  }
  if (-not $cmake -or -not (Test-Path -LiteralPath $cmake)) {
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

if ($BuildAndroidSoFlag) {
  if ([string]::IsNullOrWhiteSpace($NdkPath)) {
    $NdkPath = $env:ANDROID_NDK_HOME
  }
  if ([string]::IsNullOrWhiteSpace($NdkPath)) {
    $guess = Join-Path $env:LOCALAPPDATA "Android\Sdk\ndk"
    $NdkPath = Find-PreferredNdk $guess
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
  if (-not (Test-Path -LiteralPath $cmake)) {
    $sdk = Find-AndroidSdk
    $cmake = Find-SdkCMake $sdk
  }
  if (-not $cmake -or -not (Test-Path -LiteralPath $cmake)) { throw "CMake not found." }

  $make = Join-Path $NdkPath "prebuilt\windows-x86_64\bin\make.exe"
  if (-not (Test-Path -LiteralPath $make)) {
    throw "make.exe not found under NDK: $make"
  }

  $androidBuild = Join-Path $root ("build\android-" + $AndroidAbi)
  Ensure-Dir $androidBuild

  Run "& `"$cmake`" -S carserial/native -B `"$androidBuild`" -G `"Unix Makefiles`" -DCMAKE_MAKE_PROGRAM=`"$make`" -DANDROID_ABI=$AndroidAbi -DANDROID_PLATFORM=android-$AndroidApi -DCMAKE_TOOLCHAIN_FILE=`"$toolchain`""
  Run "& `"$cmake`" --build `"$androidBuild`" --target carserial carserial_host_tool"

  $so = Find-So $androidBuild
  if ([string]::IsNullOrWhiteSpace($so)) {
    throw "libcarserial.so not found under $androidBuild"
  }
  $dst = Join-Path $bundleDir ("android\jniLibs\" + $AndroidAbi)
  Ensure-Dir $dst
  Copy-Item -LiteralPath $so -Destination (Join-Path $dst "libcarserial.so") -Force

  $tool = Find-AndroidTool $androidBuild
  if (-not [string]::IsNullOrWhiteSpace($tool)) {
    $dstTool = Join-Path $bundleDir ("android\bin\" + $AndroidAbi)
    Ensure-Dir $dstTool
    Copy-Item -LiteralPath $tool -Destination (Join-Path $dstTool "carserial_host_tool") -Force
  } else {
    Write-Host "carserial_host_tool not found under $androidBuild (skip copy)"
  }
}

Ensure-Dir (Join-Path $bundleDir "src")
Copy-Item -LiteralPath (Join-Path $root "carserial") -Destination (Join-Path $bundleDir "src\carserial") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root "app") -Destination (Join-Path $bundleDir "src\app") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root "tools") -Destination (Join-Path $bundleDir "src\tools") -Recurse -Force

if (Test-Path -LiteralPath (Join-Path $root "doc")) {
  Copy-Item -LiteralPath (Join-Path $root "doc") -Destination (Join-Path $bundleDir "doc") -Recurse -Force
}

if ($ZipFlag) {
  $zipPath = Join-Path $out ("bundle_" + $stamp + ".zip")
  if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
  Compress-Archive -LiteralPath $bundleDir -DestinationPath $zipPath
  Write-Host "ZIP: $zipPath"
}

Write-Host "Bundle: $bundleDir"
