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
    if (-not $Condition) { throw $Message }
}

foreach ($uri in @('/', '/panel.css', '/panel.js', '/api/panel', '/api/screen.bmp')) {
    $code = $null
    try {
        $response = Invoke-WebRequest "$BaseUri$uri" -UseBasicParsing -TimeoutSec 15
        $code = [int]$response.StatusCode
    } catch {
        if ($null -eq $_.Exception.Response) { throw }
        $code = [int]$_.Exception.Response.StatusCode
    }
    Assert-Condition ($code -eq 401) "Unprotected dashboard endpoint: $uri ($code)"
}
$page = Invoke-WebRequest "$BaseUri/" -Headers $headers -UseBasicParsing -TimeoutSec 15
Assert-Condition ($page.Content.Contains('data-card="battery"')) 'Dashboard HTML not served'
$script = Invoke-WebRequest "$BaseUri/panel.js" -Headers $headers -UseBasicParsing -TimeoutSec 15
Assert-Condition (-not $script.Content.Contains('prompt(')) 'Redundant password prompt remains'
$style = Invoke-WebRequest "$BaseUri/panel.css" -Headers $headers -UseBasicParsing -TimeoutSec 15
Assert-Condition ([string]$style.Headers['Content-Type'] -match 'text/css') 'Incorrect stylesheet content type'

for ($sample = 0; $sample -lt 5; $sample++) {
    $panel = Invoke-RestMethod "$BaseUri/api/panel" -Headers $headers -TimeoutSec 15
    Assert-Condition ($panel.connected -eq $true) 'No validated PI30 identity'
    Assert-Condition ($panel.measurements_age_ms -ge 0 -and $panel.measurements_age_ms -le 20000) 'No current measurements'
    Assert-Condition (@($panel.values.PSObject.Properties).Count -eq 22) 'Wrong panel metric count'
    $values = $panel.values
    $net = $values.battery_charge_current_a - $values.battery_discharge_current_a
    Assert-Condition ([Math]::Abs($values.battery_net_current_a - $net) -lt 0.001) 'Incorrect net current'
    Assert-Condition ([Math]::Abs($values.battery_power_estimate_w - $net * $values.battery_voltage_v) -lt 0.01) 'Incorrect calculated power'
    Assert-Condition ($values.battery_soc_percent -ge 0 -and $values.battery_soc_percent -le 100) 'Invalid SOC'
    if (-not $panel.bms_connected) {
        Assert-Condition ($null -eq $values.bms_soc_percent) 'Disconnected BMS has current-looking SOC'
    }
    Assert-Condition ($null -eq $values.PSObject.Properties['ac_input_power_w']) 'Fabricated AC input power'
    $raw = Invoke-RestMethod "$BaseUri/api/axpert" -Headers $headers -TimeoutSec 15
    if ($raw.queries.QPIGS.age_ms -ge $panel.measurements_age_ms) {
        foreach ($name in @('ac_output_power_w', 'battery_soc_percent', 'battery_voltage_v', 'pv_charging_power_w')) {
            Assert-Condition ($values.$name -eq $raw.queries.QPIGS.fields.$name) "Panel differs from source: $name"
        }
    }
    Write-Output "Panel sample $sample OK: output=$($values.ac_output_power_w) W, SOC=$($values.battery_soc_percent) %, age=$($panel.measurements_age_ms) ms"
    Start-Sleep -Seconds 3
}
Write-Output 'Live dashboard checks passed, including authentication and source consistency.'
