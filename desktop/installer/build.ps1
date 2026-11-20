param(
    [Parameter(Mandatory)][string] $ElectronDirectory,
    [Parameter(Mandatory)][string] $EngineDirectory,
    [Parameter(Mandatory)][string] $Makensis,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [string] $ServiceExecutable,
    [string] $ViGEmBusInstaller,
    [switch] $WithoutViGEmBus,
    [string] $VirtualDisplayDirectory,
    [string] $SignTool,
    [switch] $KeepStaging
)

$ErrorActionPreference = 'Stop'
$desktop = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$repository = (Resolve-Path -LiteralPath (Join-Path $desktop '..')).Path
$package = Get-Content -LiteralPath (Join-Path $desktop 'package.json') -Raw | ConvertFrom-Json
$encoder = [System.Text.UTF8Encoding]::new($true)
$compiler = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$ElectronDirectory = (Resolve-Path -LiteralPath $ElectronDirectory).Path
$EngineDirectory = (Resolve-Path -LiteralPath $EngineDirectory).Path
$Makensis = (Resolve-Path -LiteralPath $Makensis).Path

function Assert-X64Executable([string] $Path) {
    $file = [System.IO.File]::OpenRead($Path)
    try {
        $reader = [System.IO.BinaryReader]::new($file)
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE executable: $Path" }
        $file.Position = 0x3C
        $file.Position = $reader.ReadUInt32()
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw "Expected an AMD64 executable: $Path"
        }
    } finally {
        $file.Dispose()
    }
}

function Copy-BundleItem([System.IO.FileSystemInfo] $Item, [string] $Destination) {
    if ($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "Release payload contains a redirected path: $($Item.FullName)"
    }
    if ($Item.PSIsContainer) {
        $target = Join-Path $Destination $Item.Name
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        foreach ($child in Get-ChildItem -LiteralPath $Item.FullName -Force) {
            Copy-BundleItem $child $target
        }
        return
    }
    Copy-Item -LiteralPath $Item.FullName -Destination $Destination
}

function Assert-TrustedDriverPackage([string] $Directory, [string] $Verifier) {
    $catalog = Join-Path $Directory 'MttVDD.cat'
    $signature = Get-AuthenticodeSignature -LiteralPath $catalog
    if ($signature.Status -ne 'Valid') {
        throw 'The optional MttVDD catalog does not have a trusted signature.'
    }
    foreach ($name in @('MttVDD.inf', 'MttVDD.dll')) {
        & $Verifier verify /pa /c $catalog (Join-Path $Directory $name)
        if ($LASTEXITCODE -ne 0) { throw "The MttVDD catalog does not verify $name." }
    }
    Write-Host 'MttVDD candidate verified. Shared virtual display support remains detection-only in this installer.'
}

if ($WithoutViGEmBus -and $ViGEmBusInstaller) { throw 'Select either a ViGEmBus payload or -WithoutViGEmBus.' }
if (-not $WithoutViGEmBus) {
    if (-not $ViGEmBusInstaller) { throw 'Supply the official ViGEmBus 1.22.0 setup or explicitly select -WithoutViGEmBus.' }
    $ViGEmBusInstaller = (Resolve-Path -LiteralPath $ViGEmBusInstaller).Path
    $expectedHash = '89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A'
    if ((Get-FileHash -LiteralPath $ViGEmBusInstaller -Algorithm SHA256).Hash -ne $expectedHash -or
        (Get-AuthenticodeSignature -LiteralPath $ViGEmBusInstaller).Status -ne 'Valid') {
        throw 'The ViGEmBus setup must match the verified 1.22.0 release.'
    }
}
if ($VirtualDisplayDirectory) {
    if (-not $SignTool) { throw 'Supply SignTool to verify the optional MttVDD package against its catalog.' }
    Assert-TrustedDriverPackage (Resolve-Path -LiteralPath $VirtualDisplayDirectory).Path (Resolve-Path -LiteralPath $SignTool).Path
}

$electron = Join-Path $ElectronDirectory 'electron.exe'
$engine = Join-Path $EngineDirectory 'sunshine.exe'
Assert-X64Executable $electron
Assert-X64Executable $engine
if ((Get-Item -LiteralPath $electron).VersionInfo.ProductVersion -ne $package.devDependencies.electron) {
    throw "The app requires the Electron $($package.devDependencies.electron) distribution."
}
if (-not (Test-Path -LiteralPath (Join-Path $EngineDirectory 'assets\apps.json') -PathType Leaf)) {
    throw 'Supply an installed or staged Sunshine engine with assets\apps.json.'
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$stageDirectory = Join-Path $output ('.sidelantern-stage-' + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $stageDirectory 'payload'
$resources = Join-Path $payload 'resources'
$app = Join-Path $resources 'app'
$engineStage = Join-Path $resources 'engine'
New-Item -ItemType Directory -Path $app,$engineStage | Out-Null

try {
    foreach ($item in Get-ChildItem -LiteralPath $ElectronDirectory -Force) {
        if ($item.Name -notin @('electron.exe', 'resources')) { Copy-BundleItem $item $payload }
    }
    Copy-Item -LiteralPath $electron -Destination (Join-Path $payload 'SideLantern.exe')
    $electronResources = Join-Path $ElectronDirectory 'resources'
    if (Test-Path -LiteralPath $electronResources -PathType Container) {
        foreach ($item in Get-ChildItem -LiteralPath $electronResources -Force) {
            if ($item.Name -notin @('default_app.asar', 'app', 'app.asar', 'app.asar.unpacked')) {
                Copy-BundleItem $item $resources
            }
        }
    }
    foreach ($name in @('main.cjs', 'preload.cjs', 'package.json', 'lib', 'assets')) {
        Copy-BundleItem (Get-Item -LiteralPath (Join-Path $desktop $name)) $app
    }
    $renderer = Join-Path $app 'renderer'
    New-Item -ItemType Directory -Path $renderer | Out-Null
    foreach ($name in @('index.html', 'styles.css', 'i18n.js', 'app.js')) {
        Copy-BundleItem (Get-Item -LiteralPath (Join-Path $desktop "renderer\$name")) $renderer
    }
    $engineAssets = Join-Path $engineStage 'assets'
    New-Item -ItemType Directory -Path $engineAssets | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath (Join-Path $EngineDirectory 'assets') -Force) {
        if ($item.Name -ne 'web') { Copy-BundleItem $item $engineAssets }
    }
    foreach ($item in Get-ChildItem -LiteralPath $EngineDirectory -File) {
        if ($item.Name -eq 'sunshine.exe' -or $item.Extension -eq '.dll') { Copy-BundleItem $item $engineStage }
    }
    Copy-Item -LiteralPath (Join-Path $repository 'LICENSE') -Destination (Join-Path $engineStage 'LICENSE-Sunshine.txt')
    if ($ServiceExecutable) {
        $ServiceExecutable = (Resolve-Path -LiteralPath $ServiceExecutable).Path
        Assert-X64Executable $ServiceExecutable
        Copy-Item -LiteralPath $ServiceExecutable -Destination (Join-Path $resources 'SideLanternService.exe')
    } else {
        & (Join-Path $desktop 'native\service\build.ps1') -OutFile (Join-Path $resources 'SideLanternService.exe')
        if ($LASTEXITCODE -ne 0) { throw 'SideLanternService could not be built.' }
    }
    & $compiler /nologo /platform:x64 /target:exe "/out:$(Join-Path $resources 'ProbeVirtualDisplay.exe')" (Join-Path $desktop 'native\ProbeVirtualDisplay.cs')
    if ($LASTEXITCODE -ne 0) { throw 'ProbeVirtualDisplay could not be built.' }
    & $compiler /nologo /platform:x64 /target:exe /r:System.ServiceProcess.dll /r:Microsoft.CSharp.dll "/out:$(Join-Path $resources 'SetupSupport.exe')" (Join-Path $PSScriptRoot 'SetupSupport.cs')
    if ($LASTEXITCODE -ne 0) { throw 'SideLantern Setup helper could not be built.' }

    $cleanup = Join-Path $stageDirectory 'payload-cleanup.nsh'
    $files = Get-ChildItem -LiteralPath $payload -File -Recurse
    $directories = Get-ChildItem -LiteralPath $payload -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending
    $commands = foreach ($file in $files) {
        $path = $file.FullName.Substring($payload.Length).Replace('$', '$$')
        '  Delete "$INSTDIR' + $path + '"'
    }
    $removeDirectories = foreach ($directory in $directories) {
        $path = $directory.FullName.Substring($payload.Length).Replace('$', '$$')
        '  RMDir "$INSTDIR' + $path + '"'
    }
    [System.IO.File]::WriteAllLines($cleanup, [string[]](@('!macro DeletePayload') + $commands + $removeDirectories + '!macroend'), $encoder)

    $setup = Join-Path $output "SideLantern-$($package.version)-win-x64-setup.exe"
    $version = ($package.version -split '[-+]')[0] + '.0'
    $argsForNsis = @('/V3', "/DPRODUCT_VERSION=$($package.version)", "/DVERSION_NUMBER=$version",
        "/DPAYLOAD=$payload", "/DPAYLOAD_CLEANUP=$cleanup", "/DSETUP_EXE=$setup")
    if (-not $WithoutViGEmBus) { $argsForNsis += "/DVIGEM_SETUP=$ViGEmBusInstaller" }
    & $Makensis @argsForNsis (Join-Path $PSScriptRoot 'SideLantern.nsi')
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $setup -PathType Leaf)) {
        throw "NSIS setup could not be built (exit code $LASTEXITCODE)."
    }
    Get-Item -LiteralPath $setup
} finally {
    if ($KeepStaging) {
        Write-Host "Staging files: $stageDirectory"
    } elseif (Test-Path -LiteralPath $stageDirectory -PathType Container) {
        $resolved = (Resolve-Path -LiteralPath $stageDirectory).Path
        if ($resolved -eq $stageDirectory -and (Split-Path -Parent $resolved) -eq $output) {
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
}
