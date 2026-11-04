param(
    [string]$Port = 'COM3',
    [string]$ProbeSerial,
    [switch]$TokenStdin
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$pythonPath = Join-Path $projectRoot '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $pythonPath)) {
    throw 'Create .venv and install pyserial before starting the website relay. See docs/cloud-hosting.md.'
}

function Find-CubeTool([string]$Pattern) {
    $cubeRoots = @(Get-ChildItem -Path 'C:\ST\STM32CubeIDE_*' -Directory -ErrorAction SilentlyContinue)
    foreach ($cubeRoot in ($cubeRoots | Sort-Object FullName -Descending)) {
        $pluginsPath = Join-Path $cubeRoot.FullName 'STM32CubeIDE\plugins'
        $toolPath = Get-Item -Path (Join-Path $pluginsPath $Pattern) -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($toolPath) { return $toolPath.FullName }
    }
    throw "STM32CubeIDE tool not found: $Pattern"
}

if (-not $env:STM32_PROGRAMMER) {
    $env:STM32_PROGRAMMER = Find-CubeTool '*mcu.externaltools.cubeprogrammer*\tools\bin\STM32_Programmer_CLI.exe'
}
if (-not $env:STM32_OPENOCD) {
    $env:STM32_OPENOCD = Find-CubeTool '*mcu.externaltools.openocd*\tools\bin\openocd.exe'
}
if (-not $env:STM32_OPENOCD_SCRIPTS) {
    $env:STM32_OPENOCD_SCRIPTS = Find-CubeTool '*mcu.debug.openocd_*\resources\openocd\st_scripts'
}
if (-not (Test-Path -LiteralPath (Join-Path $env:STM32_OPENOCD_SCRIPTS 'interface\stlink-dap.cfg'))) {
    throw 'STM32_OPENOCD_SCRIPTS must point to the MCU st_scripts directory containing interface/stlink-dap.cfg.'
}
if (-not (Get-Command arm-none-eabi-objdump -ErrorAction SilentlyContinue)) {
    $armTool = Find-CubeTool '*gnu-tools*\tools\bin\arm-none-eabi-objdump.exe'
    $env:PATH = "$(Split-Path -Parent $armTool);$env:PATH"
}
if (-not (Get-Command sh -ErrorAction SilentlyContinue)) {
    $gitBin = Join-Path $env:ProgramFiles 'Git\bin'
    if (-not (Test-Path -LiteralPath (Join-Path $gitBin 'sh.exe'))) {
        throw 'Git for Windows sh.exe is required for firmware verification.'
    }
    $env:PATH = "$gitBin;$env:PATH"
}

$relayArguments = @('-u', 'tools\cloud_relay.py', '--outputs-only', '--oled', '--uart-port', $Port)
if ($ProbeSerial) { $relayArguments += @('--oled-serial', $ProbeSerial) }
if ($TokenStdin) {
    $relayArguments += '--token-stdin'
} elseif (-not $env:GARDEN_GUARD_RELAY_TOKEN) {
    $relayArguments += '--token-prompt'
}

Push-Location $projectRoot
try {
    & $pythonPath @relayArguments
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
