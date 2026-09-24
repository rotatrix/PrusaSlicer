param([ValidateSet('deps', 'build', 'package', 'all')][string]$Stage = 'all')
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
Set-Location $repo
$depsBuild = if ($env:DEPS_BUILD_DIR) { $env:DEPS_BUILD_DIR } else { "$repo/deps/build-ci" }
$appBuild = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { "$repo/build-ci" }
$dist = if ($env:DIST_DIR) { $env:DIST_DIR } else { "$repo/dist" }

if ($Stage -in @('deps', 'all')) {
    cmake -S deps -B $depsBuild -G 'Visual Studio 17 2022' -A x64 `
        -DCMAKE_BUILD_TYPE=Release -DDEP_DEBUG=OFF -DDEP_MAX_THREADS=2 `
        -DPrusaSlicer_deps_PACKAGE_EXCLUDES=OCCT
    cmake --build $depsBuild --config Release --parallel 2
}
if ($Stage -in @('build', 'all')) {
    cmake -S . -B $appBuild -G 'Visual Studio 17 2022' -A x64 `
        "-DCMAKE_PREFIX_PATH=$depsBuild/destdir/usr/local" `
        -DCMAKE_BUILD_TYPE=Release -DSLIC3R_STATIC=ON -DSLIC3R_OPENAXIS=ON `
        -DOPENAXIS_SOURCE_DIR= -DSLIC3R_ENABLE_FORMAT_STEP=OFF `
        -DSLIC3R_ENABLE_WIN10_MESH_REPAIR=OFF -DSLIC3R_PCH=OFF `
        -DSLIC3R_BUILD_TESTS=OFF -DSLIC3R_RELEASE_DEBUG_SYMBOLS=OFF
    cmake --build $appBuild --config Release --parallel 2
    cmake -S tests/openaxis -B "$appBuild/openaxis-checks" -G 'Visual Studio 17 2022' -A x64
    cmake --build "$appBuild/openaxis-checks" --config Release --parallel 2
    ctest --test-dir "$appBuild/openaxis-checks" -C Release --output-on-failure
}
if ($Stage -in @('package', 'all')) {
    $revision = (git rev-parse --short=12 HEAD).Trim()
    $packageName = "PrusaSlicer-Rotatrix-windows-x64-$revision"
    $package = "$dist/$packageName"
    if (Test-Path -LiteralPath $package) { throw "Use a fresh package directory: $package" }
    New-Item -ItemType Directory -Path $package -Force | Out-Null
    $binaries = "$appBuild/src/slic3r-app-launcher/Release"
    Copy-Item -LiteralPath "$binaries/slic3r-app-launcher.exe" -Destination $package
    foreach ($dll in @('libgmp-10.dll', 'libmpfr-4.dll', 'WebView2Loader.dll')) {
        Copy-Item -LiteralPath "$binaries/$dll" -Destination $package
    }
    # Copy the source tree, not the build's resources junction.
    Copy-Item -LiteralPath "$repo/resources" -Destination "$package/resources" -Recurse
    Copy-Item -LiteralPath "$repo/LICENSE" -Destination $package
    $notices = "$package/third-party-notices"
    foreach ($source in @("$repo/bundled_deps", $depsBuild, "$appBuild/_deps")) {
        if (Test-Path -LiteralPath $source) {
            Get-ChildItem -LiteralPath $source -File -Recurse |
                Where-Object { $_.Name -match '^(LICENSE|COPYING|NOTICE)([.-].*)?$' } |
                ForEach-Object {
                    $relative = [System.IO.Path]::GetRelativePath($repo, $_.FullName)
                    $target = Join-Path $notices $relative
                    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
                    Copy-Item -LiteralPath $_.FullName -Destination $target
                }
        }
    }
    $sdkRevision = (git -C "$appBuild/_deps/openaxis-src" rev-parse HEAD).Trim()
    @"
PrusaSlicer - Rotatrix preview build (Windows x64)
Unofficial build; no STEP import or Windows mesh repair. Unsigned.
Source: https://github.com/rotatrix/PrusaSlicer/tree/$((git rev-parse HEAD).Trim())
OpenAxis: https://github.com/rotatrix/openaxis/tree/$sdkRevision

Extract the whole ZIP and run slic3r-app-launcher.exe.
Requires the Microsoft Visual C++ x64 Redistributable:
https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist
Browser features require Microsoft Edge WebView2 Runtime:
https://developer.microsoft.com/en-us/microsoft-edge/webview2/
OpenAxis requires Rotatrix 1.6.0 or newer. Device/GUI testing is still required.
"@ | Set-Content -LiteralPath "$package/README.txt" -Encoding utf8
    # Run from the staged folder to catch missing copied runtime files.
    Push-Location $package
    try { & './slic3r-app-launcher.exe' --datadir "$appBuild/smoke-datadir" --help } finally { Pop-Location }
    Compress-Archive -LiteralPath $package -DestinationPath "$dist/$packageName.zip"
    $hash = (Get-FileHash -LiteralPath "$dist/$packageName.zip" -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $packageName.zip" | Set-Content -LiteralPath "$dist/$packageName.zip.sha256" -Encoding ascii
}
