param(
    [Parameter(Mandatory)]
    [System.Management.Automation.PSCredential]$Credential,
    [string]$BaseUri = 'http://esp32axpertvmii.local'
)

$ErrorActionPreference = 'Stop'
$pair = '{0}:{1}' -f $Credential.UserName, $Credential.GetNetworkCredential().Password
$headers = @{ Authorization = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pair)) }
$pair = $null

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw $Message
    }
}

$status = Invoke-RestMethod "$BaseUri/api/status" -Headers $headers -TimeoutSec 15
Assert-Condition ($status.version -eq '0.4.2') 'Unexpected firmware version'
Assert-Condition ($status.hostname -eq 'esp32axpertvmii.local') 'Unexpected hostname'

$baseQueries = @(
    'QPI', 'QPIGS', 'QMOD', 'QPIWS', 'QBMS', 'QPIRI', 'QID', 'QSID',
    'QVFW', 'QVFW3', 'QFLAG', 'QDI', 'QMCHGCR', 'QMUCHGCR', 'QOPPT',
    'QCHPT', 'QT', 'QMN', 'QGMN', 'QBEQI', 'QET', 'QLT', 'QWFS',
    'QDOP', 'QVFW2', 'VERFW:'
)
$deadline = [DateTime]::UtcNow.AddSeconds(90)
do {
    $data = Invoke-RestMethod "$BaseUri/api/axpert" -Headers $headers -TimeoutSec 15
    $expected = @($baseQueries)
    if ($data.queries.QT.raw -match '^\((\d{4})(\d{2})(\d{2})\d{6}$') {
        $year = $Matches[1]
        $month = $Matches[2]
        $day = $Matches[3]
        $expected += @("QEY$year", "QEM$year$month", "QED$year$month$day",
                       "QLY$year", "QLM$year$month", "QLD$year$month$day")
    }
    $pending = @($expected | Where-Object { $data.queries.$_.attempts -lt 1 })
    if ($pending.Count -eq 0) {
        break
    }
    Start-Sleep -Seconds 3
} while ([DateTime]::UtcNow -lt $deadline)

foreach ($command in $expected) {
    $entry = $data.queries.$command
    Assert-Condition ($null -ne $entry) "Missing query $command"
    Assert-Condition ($entry.attempts -gt 0) "Query not attempted: $command"
    Assert-Condition ($entry.status -in @('ok', 'waiting', 'unsupported_nak')) "Query failed: $command ($($entry.status))"
    if ($entry.status -eq 'unsupported_nak') {
        Assert-Condition ($entry.stale -eq $true) "NAK must not look like current data: $command"
        Assert-Condition ($entry.failures -gt 0) "NAK not counted: $command"
    } elseif ($entry.valid_responses -gt 0) {
        Assert-Condition ($entry.raw.StartsWith('(')) "Invalid raw response: $command"
        Assert-Condition ($entry.tokens.Count -gt 0) "Missing tokens: $command"
        Assert-Condition ($entry.decoded -eq $true) "Unexpected payload schema: $command"
        Assert-Condition ($entry.age_ms -ge 0) "Invalid response age: $command"
    }
}

$general = $data.queries.QPIGS
Assert-Condition ($general.valid_responses -gt 0) 'No valid QPIGS response'
$generalFields = @{
    ac_input_voltage_v = 0; ac_input_frequency_hz = 1; ac_output_voltage_v = 2
    ac_output_frequency_hz = 3; ac_output_apparent_power_va = 4; ac_output_power_w = 5
    ac_output_load_percent = 6; dc_bus_voltage_v = 7; battery_voltage_v = 8
    battery_charge_current_a = 9; battery_soc_percent = 10; inverter_temperature_c = 11
    pv_input_current_a = 12; pv_input_voltage_v = 13; solar_charger_battery_voltage_v = 14
    battery_discharge_current_a = 15; pv_charging_power_w = 19
}
$culture = [Globalization.CultureInfo]::InvariantCulture
foreach ($name in $generalFields.Keys) {
    $number = [double]::Parse($general.tokens[$generalFields[$name]], $culture)
    Assert-Condition ($general.fields.$name -eq $number) "QPIGS field mismatch: $name"
}
Assert-Condition ($general.fields.battery_soc_percent -le 100) 'Invalid battery percentage'

$bms = $data.queries.QBMS
if ($bms.valid_responses -gt 0) {
    $voltageNames = @('bms_charge_voltage_v', 'bms_float_voltage_v', 'bms_cutoff_voltage_v')
    for ($index = 0; $index -lt 3; ++$index) {
        $expectedVoltage = [double]::Parse($bms.tokens[$index + 5], $culture) / 10
        Assert-Condition ($bms.fields.($voltageNames[$index]) -eq $expectedVoltage) 'QBMS voltage scale mismatch'
    }
}

$status = Invoke-RestMethod "$BaseUri/api/status" -Headers $headers -TimeoutSec 15
Assert-Condition ($status.hid -and $status.axpert) 'Inverter lost connection'
$data.queries.PSObject.Properties | ForEach-Object {
    [PSCustomObject]@{
        Query = $_.Name
        Status = $_.Value.status
        Valid = $_.Value.valid_responses
        Failures = $_.Value.failures
    }
}
Write-Host 'PASS: all read queries attempted, decoded values match raw responses, BMS units and USB connection verified.'
