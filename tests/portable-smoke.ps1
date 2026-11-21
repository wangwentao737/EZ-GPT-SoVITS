param(
    [string]$Portable = (Join-Path $PSScriptRoot '../out/portable')
)

$ErrorActionPreference = 'Stop'
$portablePath = (Resolve-Path -LiteralPath $Portable).Path
$program = Join-Path $portablePath 'EZ-GPT-SoVITS.exe'
$scratchName = 'smoke-' + [guid]::NewGuid().ToString('N')
$scratch = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.test/$scratchName"))

function Invoke-Desktop([string]$Arguments) {
    $process = Start-Process -FilePath $program -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(30000)) {
        Stop-Process -Id $process.Id
        throw "Desktop verification timed out: $Arguments"
    }
    return $process.ExitCode
}

foreach ($file in @('EZ-GPT-SoVITS.exe', 'Qt6Core.dll', 'msvcp140.dll', 'vcruntime140.dll',
    'plugins/platforms/qwindows.dll', 'plugins/sqldrivers/qsqlite.dll', 'plugins/multimedia/ffmpegmediaplugin.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $portablePath $file) -PathType Leaf)) {
        throw "Missing packaged dependency: $file"
    }
}

$previousPath = $env:PATH
$previousPlatform = $env:QT_QPA_PLATFORM
$env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
$env:QT_QPA_PLATFORM = 'windows'
try {
    $dataDirectory = Join-Path $scratch 'data'
    $data = '--data-root "{0}"' -f $dataDirectory
    if ((Invoke-Desktop "$data --smoke-test") -ne 0) {
        throw 'Portable desktop did not start with a system-only PATH.'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $dataDirectory 'library.sqlite'))) {
        throw 'The desktop did not initialize its local database.'
    }

    $engine = Join-Path $scratch 'engine (test)'
    $runtime = Join-Path $engine 'runtime'
    New-Item -ItemType Directory -Path $runtime -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $runtime 'python.exe') -Value 'test sentinel'
    Set-Content -LiteralPath (Join-Path $engine 'webui.py') -Value '# test sentinel'
    Set-Content -LiteralPath (Join-Path $engine 'config.py') -Value '# test sentinel'
    $sentinel = Join-Path $engine 'unexpected-launch.txt'
    $launcher = Join-Path $engine 'go-webui.bat'
    Set-Content -LiteralPath $launcher -Value '@echo started>unexpected-launch.txt' -Encoding ascii

    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 9874)
    $listener.Start()
    try {
        if ((Invoke-Desktop "$data --probe-engine `"$launcher`"") -ne 1) {
            throw 'The desktop did not report the occupied engine port.'
        }
        if (-not $listener.Server.IsBound) {
            throw 'The existing listener was closed.'
        }
    } finally {
        $listener.Stop()
    }

    $alias = Join-Path $scratch 'engine-junction'
    New-Item -ItemType Junction -Path $alias -Target $engine | Out-Null
    $aliasLauncher = Join-Path $alias 'go-webui.bat'
    if ((Invoke-Desktop "$data --probe-engine `"$aliasLauncher`"") -ne 1) {
        throw 'An engine path through a junction was accepted.'
    }
    if (Test-Path -LiteralPath $sentinel) {
        throw 'The test launcher ran during a rejection case.'
    }
    Write-Output 'Portable launch, SQLite setup, port conflict, and engine junction rejection passed.'
} finally {
    $env:PATH = $previousPath
    $env:QT_QPA_PLATFORM = $previousPlatform
}
