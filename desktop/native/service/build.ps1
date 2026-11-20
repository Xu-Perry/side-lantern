param(
    [Parameter(Mandatory = $true)]
    [string] $OutFile,
    [switch] $IncludeSelfTests
)

$ErrorActionPreference = 'Stop'
$framework = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319'
$compiler = Join-Path $framework 'csc.exe'
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw 'The x64 .NET Framework C# compiler is required to build SideLanternService.'
}
$target = [System.IO.Path]::GetFullPath($OutFile)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($target)) | Out-Null
$sourceSet = Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.cs' -File |
    Sort-Object Name | ForEach-Object { $_.FullName }
$protectedPathSource = Join-Path (Split-Path -Parent $PSScriptRoot) 'ProtectedInstallPath.cs'
$serviceAssembly = Join-Path $framework 'System.ServiceProcess.dll'
$testSwitches = @()
if ($IncludeSelfTests) { $testSwitches += '/define:HOST_SERVICE_SELF_TEST' }
& $compiler /nologo /target:exe /platform:x64 /warn:4 /warnaserror+ "/r:$serviceAssembly" "/out:$target" @testSwitches @sourceSet $protectedPathSource
if ($LASTEXITCODE -ne 0) { throw "SideLanternService compilation failed ($LASTEXITCODE)." }
Get-Item -LiteralPath $target
