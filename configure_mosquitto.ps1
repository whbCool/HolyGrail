# configure_mosquitto.ps1
# Requires Administrative privileges to run.
# Right-click PowerShell and select "Run as Administrator", then execute this script.

$ErrorActionPreference = "Stop"

# 1. Modify mosquitto.conf
$confPath = "C:\Program Files\Mosquitto\mosquitto.conf"
if (Test-Path $confPath) {
    $content = Get-Content $confPath -Raw
    if ($content -notmatch "listener\s+1883") {
        Write-Host "[+] Configuring Mosquitto to allow external connections..." -ForegroundColor Green
        # Append listener and allow_anonymous configuration
        Add-Content -Path $confPath -Value "`n# Configured by Antigravity`nlistener 1883 0.0.0.0`nallow_anonymous true`n"
    } else {
        Write-Host "[*] Mosquitto is already configured with a listener." -ForegroundColor Yellow
    }
} else {
    Write-Error "[-] Mosquitto configuration file not found at $confPath"
    exit 1
}

# 2. Restart Mosquitto service
Write-Host "[+] Restarting Mosquitto Broker service..." -ForegroundColor Green
Restart-Service -Name mosquitto
$service = Get-Service -Name mosquitto
Write-Host "[*] Mosquitto service status: $($service.Status)" -ForegroundColor Green

# 3. Check TCP port
Write-Host "[+] Verifying listener port 1883..." -ForegroundColor Green
Start-Sleep -Seconds 1
$conn = Get-NetTCPConnection -LocalPort 1883 -ErrorAction SilentlyContinue
if ($conn) {
    Write-Host "[+] Mosquitto is listening successfully on Port 1883!" -ForegroundColor Green
    $conn | Format-Table LocalAddress, LocalPort, State
} else {
    Write-Warning "[-] Could not detect active listener on Port 1883. Check Mosquitto service logs."
}

# 4. Optional Ethernet Configuration for Direct Board Connection (Option B)
Write-Host ""
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "OPTIONAL ETHERNET CONFIGURATION (FOR DIRECT BOARD CONNECTION)" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
$choice = Read-Host "Do you want to configure your PC's Ethernet port to static IP 192.168.2.10? (Y/N)"
if ($choice -eq 'Y' -or $choice -eq 'y') {
    Write-Host "[+] Searching for Ethernet adapter..." -ForegroundColor Green
    $eth = Get-NetAdapter | Where-Object { $_.Name -eq "Ethernet" -and $_.Status -ne "Disabled" }
    if ($eth) {
        Write-Host "[+] Found Ethernet interface. Configuring IP 192.168.2.10..." -ForegroundColor Green
        
        # Disable DHCP and remove existing IPv4 addresses if any
        Set-NetIPInterface -InterfaceAlias Ethernet -DHCP Disabled -ErrorAction SilentlyContinue
        Remove-NetIPAddress -InterfaceAlias Ethernet -AddressFamily IPv4 -Confirm:$false -ErrorAction SilentlyContinue
        
        # Assign static IP and gateway
        New-NetIPAddress -InterfaceAlias Ethernet -IPAddress 192.168.2.10 -PrefixLength 24 -DefaultGateway 192.168.2.1
        
        Write-Host "[+] Ethernet IP configured successfully!" -ForegroundColor Green
        Get-NetIPAddress -InterfaceAlias Ethernet -AddressFamily IPv4 | Format-Table IPAddress, PrefixLength, PrefixOrigin
    } else {
        Write-Error "[-] Ethernet adapter named 'Ethernet' not found or is disabled."
    }
} else {
    Write-Host "[*] Skipped Ethernet adapter configuration." -ForegroundColor Yellow
}
