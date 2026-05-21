# Launch the WASM build in Chrome with remote debugging and capture all
# browser-console output during a fresh page load via the DevTools protocol.
$ErrorActionPreference = 'Continue'
$chrome  = "C:\Program Files\Google\Chrome\Application\chrome.exe"
$prof    = "C:\Users\nedch\AppData\Local\Temp\chrome-wasm"
$port    = 9223
$url     = "http://localhost:8778/la360.html"

Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" |
    Where-Object { $_.CommandLine -like '*chrome-wasm*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 2

Start-Process $chrome -ArgumentList "--user-data-dir=$prof",
    "--remote-debugging-port=$port","--new-window","about:blank"
Start-Sleep 6

$page = (Invoke-RestMethod "http://localhost:$port/json") |
    Where-Object { $_.type -eq 'page' } | Select-Object -First 1
if (-not $page) { Write-Output "no page target"; exit }

$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ct = [System.Threading.CancellationToken]::None
$ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, $ct).Wait()

function Send-Cmd($id, $method, $params) {
    $o = @{ id=$id; method=$method }
    if ($params) { $o.params = $params }
    $j = $o | ConvertTo-Json -Depth 10 -Compress
    $b = [System.Text.Encoding]::UTF8.GetBytes($j)
    $ws.SendAsync([System.ArraySegment[byte]]::new($b), 'Text', $true, $ct).Wait()
}
Send-Cmd 1 'Runtime.enable' $null
Send-Cmd 2 'Log.enable' $null
Send-Cmd 3 'Page.enable' $null
Send-Cmd 4 'Page.navigate' @{ url = $url }

$deadline = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $deadline) {
    $sb  = New-Object System.Text.StringBuilder
    $eom = $false
    $gotMsg = $true
    do {
        $buf = New-Object byte[] 131072
        $res = $ws.ReceiveAsync([System.ArraySegment[byte]]::new($buf), $ct)
        if (-not $res.Wait(3000)) { $gotMsg = $false; break }
        [void]$sb.Append([System.Text.Encoding]::UTF8.GetString($buf,0,$res.Result.Count))
        $eom = $res.Result.EndOfMessage
    } while (-not $eom)
    if (-not $gotMsg) { continue }
    try { $m = $sb.ToString() | ConvertFrom-Json } catch { continue }
    if ($m.method -eq 'Runtime.consoleAPICalled') {
        $parts = @($m.params.args | ForEach-Object { if ($_.value -ne $null) { [string]$_.value } elseif ($_.description) { $_.description } else { $_.type } })
        Write-Output ("[console." + $m.params.type + "] " + ($parts -join ' '))
    } elseif ($m.method -eq 'Runtime.exceptionThrown') {
        $ed = $m.params.exceptionDetails
        $txt = $ed.text
        if ($ed.exception -and $ed.exception.description) { $txt += " :: " + $ed.exception.description }
        Write-Output ("[EXCEPTION] " + $txt)
    } elseif ($m.method -eq 'Log.entryAdded') {
        Write-Output ("[log." + $m.params.entry.level + "] " + $m.params.entry.text)
    }
}
$ws.CloseAsync('NormalClosure','',$ct).Wait()
Write-Output "=== capture done ==="
