param(
    [string]$Compiler = 'cc',
    [switch]$Zig,
    [string]$EspIdfPath = "$env:USERPROFILE\.platformio\packages\framework-espidf"
)

$ErrorActionPreference = 'Stop'
$json = Join-Path $EspIdfPath 'components\json\cJSON'
$output = Join-Path ([IO.Path]::GetTempPath()) ("axpert-settings-{0}.exe" -f [guid]::NewGuid())
$arguments = @()
if ($Zig) { $arguments += 'cc' }
$arguments += @('-std=c11', '-Wall', '-Wextra', '-Werror',
    '-I', (Join-Path $PSScriptRoot 'settings-host'),
    '-I', (Join-Path $PSScriptRoot '..\main'), '-I', $json,
    (Join-Path $PSScriptRoot 'inverter_settings.test.c'),
    (Join-Path $PSScriptRoot '..\main\telemetry.c'),
    (Join-Path $PSScriptRoot '..\main\inverter_settings.c'),
    (Join-Path $json 'cJSON.c'), '-o', $output)
try {
    & $Compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Settings host test compilation failed' }
    & $output
    if ($LASTEXITCODE -ne 0) { throw 'Settings host tests failed' }
} finally {
    if (Test-Path -LiteralPath $output) {
        for ($attempt = 0; $attempt -lt 10; $attempt++) {
            try {
                Remove-Item -LiteralPath $output -ErrorAction Stop
                break
            } catch [System.IO.IOException] {
                if ($attempt -eq 9) { throw }
                Start-Sleep -Milliseconds 500
            }
        }
    }
}
