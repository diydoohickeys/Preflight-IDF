#Requires -Version 7.0
<#
.SYNOPSIS
    Gives Preflight devices (and an MQTT broker) certificates your browsers trust, from a CA that
    lives only on this computer.

.DESCRIPTION
    A Preflight device with HTTPS turned on (/setup, Access card) serves a self-signed certificate
    it makes itself, so browsers warn and refuse to cache it. This script makes a private certificate authority once, trusts
    it on this computer, and signs each device's certificate with it. The device's private key
    never leaves the device: the script fetches a signing request (CSR) for it, signs that, and
    uploads the certificate. The device restarts to serve it.

    The CA's certificate is also stored on the device, which then uses it to check the
    certificate of the MQTT broker it connects to; -Broker makes that broker certificate.

    Everything is kept in -CaDir (default ~/.preflight-ca). ca.key signs certificates every
    device you trust will accept: keep it private and back it up. Losing it means making a new
    CA and re-signing every device.

.EXAMPLE
    ./preflight-certs.ps1 -Device rackglow
    First run: makes the CA, asks to trust it, then signs rackglow's certificate.

.EXAMPLE
    ./preflight-certs.ps1 -Device rackglow, 192.168.1.40 -ApiKey 0123abcd...
    Signs two devices. The API key is on each device's /setup page (Access card); devices with
    different keys are signed one at a time, or with $env:PREFLIGHT_API_KEY set per run.

.EXAMPLE
    ./preflight-certs.ps1 -Broker homeassistant.local, 192.168.1.10
    Writes a certificate and key for the MQTT broker at those names, with setup notes.

.EXAMPLE
    ./preflight-certs.ps1 -Trust -CaFile ./ca.crt
    Trusts an existing CA on another computer (copy ca.crt only, never ca.key).
#>
[CmdletBinding(DefaultParameterSetName = 'Device')]
param(
    # Device hostnames or addresses, as you reach them in a browser.
    [Parameter(ParameterSetName = 'Device', Position = 0)]
    [string[]] $Device,

    # The device's API key (/setup, Access card). Else $env:PREFLIGHT_API_KEY, else a prompt.
    [Parameter(ParameterSetName = 'Device')]
    [string] $ApiKey,

    [Parameter(ParameterSetName = 'Device')]
    [int] $Port = 443,

    # Names and addresses the MQTT broker is reached by; the first names the output files.
    [Parameter(ParameterSetName = 'Broker', Mandatory)]
    [string[]] $Broker,

    # Install the CA into this computer's trust store (done automatically when the CA is made).
    [Parameter(ParameterSetName = 'Trust', Mandatory)]
    [switch] $Trust,

    # The CA certificate to trust; default the one in -CaDir.
    [Parameter(ParameterSetName = 'Trust')]
    [string] $CaFile,

    [string] $CaDir = (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.preflight-ca')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Apple's ceiling for TLS server certificates; Chrome and Firefox exempt private CAs from the
# shorter public-web limit.
$LeafDays = 825
$CaYears = 20

function Write-Step([string] $Text) { Write-Host "==> $Text" -ForegroundColor Cyan }

function Protect-File([string] $Path) {
    if ($IsWindows) {
        icacls $Path /inheritance:r /grant:r "$($env:USERNAME):F" | Out-Null
    } else {
        chmod 600 $Path
    }
}

function New-Serial {
    $bytes = [byte[]]::new(16)
    [System.Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    $bytes[0] = $bytes[0] -band 0x7F
    $bytes[15] = $bytes[15] -bor 0x01
    return $bytes
}

function New-P256Key {
    return [System.Security.Cryptography.ECDsa]::Create(
        [System.Security.Cryptography.ECCurve+NamedCurves]::nistP256)
}

function Get-CaPaths {
    return @{
        Cert = Join-Path $CaDir 'ca.crt'
        Key  = Join-Path $CaDir 'ca.key'
    }
}

function Install-CaTrust([string] $CertPath) {
    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::CreateFromPem((Get-Content -Raw $CertPath))
    Write-Step "Trusting '$($cert.Subject)' on this computer"
    if ($IsWindows) {
        # Windows shows its own confirmation dialog for a new root - answer Yes.
        Import-Certificate -FilePath $CertPath -CertStoreLocation Cert:\CurrentUser\Root | Out-Null
        Write-Host '    Trusted for this Windows user (Edge, Chrome; Firefox via its enterprise-roots setting, on by default).'
    } elseif ($IsMacOS) {
        security add-trusted-cert -r trustRoot -k "$HOME/Library/Keychains/login.keychain-db" $CertPath
        Write-Host '    Trusted in your login keychain (Safari, Chrome).'
    } else {
        Write-Warning "Trust it with your distribution's tool, e.g. on Debian/Ubuntu:"
        Write-Host "    sudo cp '$CertPath' /usr/local/share/ca-certificates/preflight-ca.crt && sudo update-ca-certificates"
        Write-Host '    Firefox and Chrome on Linux keep their own stores: import it under Settings > Certificates.'
    }
    Write-Host '    Phones and other computers: copy ca.crt (never ca.key) and install it as a trusted CA there.'
}

function Get-Ca {
    $paths = Get-CaPaths
    if ((Test-Path $paths.Cert) -and (Test-Path $paths.Key)) {
        return [System.Security.Cryptography.X509Certificates.X509Certificate2]::CreateFromPemFile($paths.Cert, $paths.Key)
    }
    if ((Test-Path $paths.Cert) -or (Test-Path $paths.Key)) {
        throw "Half a CA in $CaDir (ca.crt and ca.key go together). Restore the missing file, or move both away to start a new CA."
    }

    Write-Step "Making your CA in $CaDir"
    New-Item -ItemType Directory -Force -Path $CaDir | Out-Null
    $key = New-P256Key
    $name = "CN=Preflight Local CA ($([Environment]::MachineName))"
    $req = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new($name, $key, 'SHA256')
    $req.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($true, $true, 0, $true))
    $req.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new('KeyCertSign, CrlSign', $true))
    $req.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509SubjectKeyIdentifierExtension]::new($req.PublicKey, $false))
    $now = [DateTimeOffset]::UtcNow
    $ca = $req.CreateSelfSigned($now.AddDays(-1), $now.AddYears($CaYears))

    # The key is written first and locked down before the certificate makes the pair usable.
    Set-Content -Path $paths.Key -Value $key.ExportPkcs8PrivateKeyPem() -NoNewline
    Protect-File $paths.Key
    Set-Content -Path $paths.Cert -Value $ca.ExportCertificatePem() -NoNewline
    Write-Host "    ca.crt - share freely; ca.key - private, back it up"

    Install-CaTrust $paths.Cert
    return [System.Security.Cryptography.X509Certificates.X509Certificate2]::CreateFromPemFile($paths.Cert, $paths.Key)
}

function Add-Names($Builder, [string[]] $Names) {
    $listed = [System.Collections.Generic.List[string]]::new()
    foreach ($n in $Names) {
        if ([string]::IsNullOrWhiteSpace($n) -or $listed.Contains($n.ToLowerInvariant())) { continue }
        $listed.Add($n.ToLowerInvariant())
        $ip = $null
        if ([System.Net.IPAddress]::TryParse($n, [ref] $ip)) { $Builder.AddIpAddress($ip) } else { $Builder.AddDnsName($n) }
    }
    return , $listed
}

function New-LeafCertificate($Ca, $Request, [string[]] $Names) {
    $san = [System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
    $listed = Add-Names $san $Names
    $Request.CertificateExtensions.Add($san.Build($false))
    $Request.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
    $Request.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new('DigitalSignature', $true))
    $eku = [System.Security.Cryptography.OidCollection]::new()
    [void] $eku.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.1'))
    $Request.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($eku, $false))
    $Request.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509SubjectKeyIdentifierExtension]::new($Request.PublicKey, $false))
    $Request.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509AuthorityKeyIdentifierExtension]::CreateFromCertificate($Ca, $true, $false))

    $now = [DateTimeOffset]::UtcNow
    $notAfter = $now.AddDays($LeafDays)
    $caEnd = [DateTimeOffset]::new($Ca.NotAfter.ToUniversalTime())
    if ($notAfter -gt $caEnd) { $notAfter = $caEnd }
    $cert = $Request.Create($Ca, $now.AddHours(-1), $notAfter, (New-Serial))
    Write-Host "    names: $($listed -join ', '); valid to $($notAfter.ToString('yyyy-MM-dd'))"
    return $cert
}

function Get-ServedCertificate([string] $HostName, [int] $TcpPort) {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $client.ConnectAsync($HostName, $TcpPort).Wait(5000) | Out-Null
        if (-not $client.Connected) { return $null }
        $ssl = [System.Net.Security.SslStream]::new($client.GetStream(), $false, { $true })
        $ssl.AuthenticateAsClient($HostName)
        return [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($ssl.RemoteCertificate)
    } catch {
        return $null
    } finally {
        $client.Dispose()
    }
}

function Resolve-ApiKey {
    if ($ApiKey) { return $ApiKey }
    if ($env:PREFLIGHT_API_KEY) { return $env:PREFLIGHT_API_KEY }
    $secure = Read-Host -AsSecureString 'API key (the device''s /setup page, Access card)'
    return [System.Net.NetworkCredential]::new('', $secure).Password
}

function Invoke-Device([string] $Method, [string] $Url, [string] $Key, $Body) {
    # Certificate checks are off for these calls only: the device is still self-signed (or was
    # signed by a CA this computer may not trust yet). What comes back is checked instead - the
    # signed certificate must match the device's own key, which the device verifies.
    $params = @{
        Method               = $Method
        Uri                  = $Url
        Headers              = @{ Authorization = "Bearer $Key" }
        SkipCertificateCheck = $true
        TimeoutSec           = 30
    }
    if ($Body) { $params.Body = $Body; $params.ContentType = 'application/x-www-form-urlencoded' }
    try {
        return Invoke-RestMethod @params
    } catch {
        if (-not $_.Exception.Response) {
            throw "Couldn't reach $Url. Is HTTPS turned on? It's on the device's /setup page (Access card), off until you turn it on."
        }
        $status = $_.Exception.Response.StatusCode.value__
        $detail = $_.ErrorDetails.Message
        if ($status -eq 401) { throw "$Url refused the API key (401). Copy it from the device's /setup page." }
        if ($status -eq 404) { throw "$Url is missing (404). This device's firmware doesn't offer HTTPS." }
        if ($detail) { throw "$Url failed ($status): $detail" }
        throw
    }
}

function Set-DeviceCertificate($Ca, [string] $Target, [string] $Key) {
    $base = if ($Port -eq 443) { "https://$Target" } else { "https://${Target}:$Port" }
    Write-Step "Signing $Target"

    $info = Invoke-Device GET "$base/api/access/csr" $Key
    $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::LoadSigningRequestPem(
        $info.csr, [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.X509Certificates.CertificateRequestLoadOptions]::Default, $null)

    $names = @($Target, $info.hostname, "$($info.hostname).local")
    if ($info.ip) { $names += $info.ip }
    $leaf = New-LeafCertificate $Ca $request $names

    $body = @{ cert = $leaf.ExportCertificatePem(); ca = $Ca.ExportCertificatePem() }
    Invoke-Device POST "$base/api/access/certificate" $Key $body | Out-Null
    Write-Host '    installed; the device is restarting'

    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    Start-Sleep -Seconds 4
    while ([DateTime]::UtcNow -lt $deadline) {
        $served = Get-ServedCertificate $Target $Port
        if ($served -and $served.Thumbprint -eq $leaf.Thumbprint) {
            Write-Host "    $base now serves the signed certificate" -ForegroundColor Green
            return
        }
        Start-Sleep -Seconds 3
    }
    Write-Warning "$Target did not come back with the new certificate within 90 s - check its log (/logs)."
}

function New-BrokerCertificate($Ca) {
    $name = $Broker[0]
    $safe = ($name -replace '[^A-Za-z0-9.-]', '_')
    $outDir = Join-Path $CaDir "broker-$safe"
    Write-Step "Making a certificate for the MQTT broker at $($Broker -join ', ')"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    $key = New-P256Key
    $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new("CN=$name", $key, 'SHA256')
    $leaf = New-LeafCertificate $Ca $request $Broker

    $keyPath = Join-Path $outDir 'broker.key'
    Set-Content -Path $keyPath -Value $key.ExportPkcs8PrivateKeyPem() -NoNewline
    Protect-File $keyPath
    Set-Content -Path (Join-Path $outDir 'broker.crt') -Value $leaf.ExportCertificatePem() -NoNewline
    Set-Content -Path (Join-Path $outDir 'ca.crt') -Value $Ca.ExportCertificatePem() -NoNewline

    Write-Host "    written to $outDir"
    Write-Host @"

Home Assistant's Mosquitto broker add-on:
  1. Copy broker.crt and broker.key into Home Assistant's /ssl folder (Samba or File editor add-on).
  2. Mosquitto broker > Configuration: certfile: broker.crt   keyfile: broker.key
     (leave cafile empty - clients are not asked for certificates), then restart the add-on.
  3. It now also listens with TLS on port 8883; port 1883 stays plain for anything not moved yet.

Plain Mosquitto (mosquitto.conf):
  listener 8883
  certfile /path/to/broker.crt
  keyfile  /path/to/broker.key

Then on each device turn on MQTT TLS and set the port to 8883, using a host name listed above.
Devices check the broker against the CA they were given by -Device, so sign them first.
"@
}

switch ($PSCmdlet.ParameterSetName) {
    'Trust' {
        $path = if ($CaFile) { $CaFile } else { (Get-CaPaths).Cert }
        if (-not (Test-Path $path)) { throw "No CA certificate at $path." }
        Install-CaTrust $path
    }
    'Broker' {
        New-BrokerCertificate (Get-Ca)
    }
    default {
        $ca = Get-Ca
        if (-not $Device) {
            Write-Host "`nCA ready. Sign a device with: ./preflight-certs.ps1 -Device <hostname>"
            return
        }
        $key = Resolve-ApiKey
        foreach ($d in $Device) { Set-DeviceCertificate $ca $d $key }
    }
}
