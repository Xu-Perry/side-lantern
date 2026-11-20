param(
    [Parameter(Mandatory)][string] $ElectronPath,
    [Parameter(Mandatory)][string] $EngineDirectory,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [ValidateRange(1050, 65500)][int] $BasePort = 48989
)

$ErrorActionPreference = 'Stop'
$ElectronPath = (Resolve-Path -LiteralPath $ElectronPath).Path
$EngineDirectory = (Resolve-Path -LiteralPath $EngineDirectory).Path
$desktopDirectory = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$originalService = (Get-Service SunshineService -ErrorAction SilentlyContinue).Status
$previousEnvironment = @{
    SIDELANTERN_ENGINE_DIR = $env:SIDELANTERN_ENGINE_DIR
    SIDELANTERN_DATA_DIR = $env:SIDELANTERN_DATA_DIR
    SIDELANTERN_TEST_PORT = $env:SIDELANTERN_TEST_PORT
}

function Test-PrivateManagementPort {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connection = $client.ConnectAsync('127.0.0.1', ($BasePort + 1))
        return $connection.Wait(500) -and $client.Connected
    } catch {
        return $false
    } finally {
        $client.Dispose()
    }
}

try {
    if (Test-PrivateManagementPort) { throw "Test management port $($BasePort + 1) is already in use." }
    $env:SIDELANTERN_TEST_PORT = [string]$BasePort
    $env:SIDELANTERN_ENGINE_DIR = $EngineDirectory
    foreach ($mode in @('normal', 'forced')) {
        $data = Join-Path $OutputDirectory ("smoke-" + $mode + '-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $data -Force | Out-Null
        $env:SIDELANTERN_DATA_DIR = (Resolve-Path -LiteralPath $data).Path
        $stdout = Join-Path $data 'desktop-stdout.log'
        $stderr = Join-Path $data 'desktop-stderr.log'
        $arguments = @('.', '--smoke-test')
        if ($mode -eq 'forced') { $arguments += '--smoke-hold' }
        $launch = @{
            FilePath = $ElectronPath
            WorkingDirectory = $desktopDirectory
            ArgumentList = $arguments
            WindowStyle = 'Hidden'
            RedirectStandardOutput = $stdout
            RedirectStandardError = $stderr
            PassThru = $true
        }
        $process = Start-Process @launch
        try {
        $deadline = [DateTime]::UtcNow.AddSeconds(75)
            do {
                $result = Get-Content -LiteralPath $stdout -Raw
                $process.Refresh()
                if ($result -match 'SideLantern smoke passed' -or $process.HasExited) { break }
                Start-Sleep -Milliseconds 250
            } while ([DateTime]::UtcNow -lt $deadline)
            if ($result -notmatch 'SideLantern smoke passed') {
                Get-Content -LiteralPath $stderr
                throw "SideLantern did not become ready ($mode). See $data."
            }
            if ($mode -eq 'forced') {
                if ($process.HasExited -or -not (Test-PrivateManagementPort)) { throw 'Private host exited prematurely.' }
                Stop-Process -InputObject $process -Force
            }
            if (-not $process.WaitForExit(20000)) { throw "SideLantern did not exit ($mode)." }
            if ($mode -eq 'normal' -and $process.ExitCode -ne 0) { throw "SideLantern exit code $($process.ExitCode)." }
            $deadline = [DateTime]::UtcNow.AddSeconds(16)
            while ((Test-PrivateManagementPort) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 200 }
            if (Test-PrivateManagementPort) { throw "Private host remained reachable after $mode exit." }
            $engineLog = Join-Path $data 'host/logs/engine-console.log'
            if (-not (Select-String -LiteralPath $engineLog -SimpleMatch 'Interrupt handler called' -Quiet)) {
                throw "The engine did not acknowledge graceful shutdown after $mode exit. See $engineLog."
            }
            "SideLantern authenticated startup, theme and language persistence, and engine shutdown passed ($mode)."
        } finally {
            $process.Refresh()
            if (-not $process.HasExited) { Stop-Process -InputObject $process -Force }
        }
    }
    if ((Get-Service SunshineService -ErrorAction SilentlyContinue).Status -ne $originalService) {
        throw 'The original SunshineService changed status.'
    }
    "Original SunshineService state remained $originalService."
} finally {
    foreach ($name in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process')
    }
}
