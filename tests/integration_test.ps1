[CmdletBinding()]
param(
    [string]$BinaryPath = (Join-Path $PSScriptRoot "..\build\Release\fakaapache.exe")
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("fakaapache-tests-" + [guid]::NewGuid())
$serverProcess = $null

function Assert-Equal([object]$Expected, [object]$Actual, [string]$Message) {
    if ($Expected -ne $Actual) {
        throw "$Message. Expected '$Expected', received '$Actual'."
    }
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw $Message
    }
}

function New-TestConfig([int]$Port, [string[]]$ExtraLines) {
    $configPath = Join-Path $tempRoot ("siteconfig-$Port.fakaapache")
    @(
        "[server]"
        "server_name = windows-test"
        "bind_address = 127.0.0.1"
        "port = $Port"
        "root_directory = $repoRoot\www"
        "index_file = index.html"
        "rate_limit_requests_per_minute = 120"
        "rate_limit_burst = 20"
        $ExtraLines
    ) | Set-Content -LiteralPath $configPath -Encoding ASCII
    return $configPath
}

function Start-TestServer([string]$ConfigPath, [int]$Port) {
    $stdout = Join-Path $tempRoot "server-$Port.out.log"
    $stderr = Join-Path $tempRoot "server-$Port.err.log"
    $script:serverProcess = Start-Process -FilePath $BinaryPath `
        -ArgumentList @("--config", $ConfigPath) `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -WindowStyle Hidden `
        -PassThru

    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        try {
            $response = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/" -UseBasicParsing -TimeoutSec 1
            if ($response.StatusCode -ge 200) { return }
        } catch {
            Start-Sleep -Milliseconds 100
        }
        if ($serverProcess.HasExited) {
            throw "FakaApache exited while starting. See $stderr"
        }
    }
    throw "FakaApache did not start on port $Port. See $stderr"
}

function Stop-TestServer {
    if ($null -ne $script:serverProcess -and -not $script:serverProcess.HasExited) {
        Stop-Process -Id $script:serverProcess.Id -Force
        $script:serverProcess.WaitForExit()
    }
    $script:serverProcess = $null
}

function Invoke-AllowHttpError([string]$Uri, [hashtable]$Headers = @{}) {
    try {
        return Invoke-WebRequest -Uri $Uri -Headers $Headers -UseBasicParsing
    } catch {
        if ($null -ne $_.Exception.Response) {
            return $_.Exception.Response
        }
        throw
    }
}

try {
    Assert-True (Test-Path -LiteralPath $BinaryPath) "Binary not found: $BinaryPath"
    New-Item -ItemType Directory -Path $tempRoot | Out-Null

    $port = Get-Random -Minimum 18080 -Maximum 19000
    $config = New-TestConfig $port @()
    Start-TestServer $config $port

    $get = Invoke-WebRequest -Uri "http://127.0.0.1:$port/" -UseBasicParsing
    Assert-Equal 200 $get.StatusCode "GET failed"
    Assert-True ($get.Content.Contains("FakaApache Example Site")) "GET returned the wrong page"

    $head = Invoke-WebRequest -Uri "http://127.0.0.1:$port/" -Method Head -UseBasicParsing
    Assert-Equal 200 $head.StatusCode "HEAD failed"
    Assert-Equal 0 $head.Content.Length "HEAD returned a response body"

    $rangeRequest = [System.Net.HttpWebRequest]::Create("http://127.0.0.1:$port/")
    $rangeRequest.AddRange(0, 9)
    $rangeResponse = $rangeRequest.GetResponse()
    try {
        Assert-Equal 206 ([int]$rangeResponse.StatusCode) "Range request failed"
        Assert-True ($rangeResponse.Headers["Content-Range"].StartsWith("bytes 0-9/")) "Missing Content-Range header"
        $rangeBuffer = New-Object System.IO.MemoryStream
        $rangeResponse.GetResponseStream().CopyTo($rangeBuffer)
        Assert-Equal 10 $rangeBuffer.Length "Range response had the wrong size"
        $rangeBuffer.Dispose()
    } finally {
        $rangeResponse.Close()
    }

    $gzip = Invoke-WebRequest -Uri "http://127.0.0.1:$port/" -Headers @{ "Accept-Encoding" = "gzip" } -UseBasicParsing
    Assert-Equal "gzip" $gzip.Headers["Content-Encoding"] "Gzip negotiation failed"
    Stop-TestServer

    $authPort = Get-Random -Minimum 18080 -Maximum 19000
    $authConfig = New-TestConfig $authPort @("auth_username = admin", "auth_password = change-me")
    Start-TestServer $authConfig $authPort
    try {
        $unauthorized = Invoke-AllowHttpError "http://127.0.0.1:$authPort/"
        Assert-Equal 401 $unauthorized.StatusCode "Unauthenticated request was accepted"
        $token = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("admin:change-me"))
        $authorized = Invoke-WebRequest -Uri "http://127.0.0.1:$authPort/" -Headers @{ Authorization = "Basic $token" } -UseBasicParsing
        Assert-Equal 200 $authorized.StatusCode "Authenticated request failed"
    } finally {
        Stop-TestServer
    }

    $ratePort = Get-Random -Minimum 18080 -Maximum 19000
    $rateConfig = New-TestConfig $ratePort @("rate_limit_requests_per_minute = 1", "rate_limit_burst = 1")
    Start-TestServer $rateConfig $ratePort
    try {
        $first = Invoke-WebRequest -Uri "http://127.0.0.1:$ratePort/" -UseBasicParsing
        Assert-Equal 200 $first.StatusCode "First rate-limit request failed"
        $second = Invoke-AllowHttpError "http://127.0.0.1:$ratePort/"
        Assert-Equal 429 $second.StatusCode "Rate limit did not reject the second request"
    } finally {
        Stop-TestServer
    }

    Write-Output "Windows integration tests passed."
} finally {
    Stop-TestServer
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
