# Test-HttpServer.ps1 — 极简 LAN HTTP 服务,给 Lumia 真机的 WebCoreLoadUrl 当测试目标。
# 监听 0.0.0.0:<Port>,对任意请求返回一个"明显来自网络"的英文页面(区别于本地回退页)。
param([int]$Port=8080)
$ErrorActionPreference='Continue'
$html = @'
<!DOCTYPE html><html><head><meta charset="utf-8"></head>
<body style="margin:0;background:#ffffff;font-family:sans-serif">
<h1 style="background:#cc0033;color:#ffffff;margin:0;padding:24px;font-size:40px">LIVE HTTP FROM LAN PC</h1>
<p style="color:#222222;font-size:30px;padding:18px;margin:0">If you can read this on the Lumia 950, the full network path works: curl + in-process loader bridge + WebCore layout/paint over real HTTP.</p>
<div style="margin:20px;padding:22px;background:#00aa88;color:#ffffff;font-size:30px;border-radius:14px">NETWORK STACK OK &middot; ARM32 &middot; Win10M</div>
<p style="color:#888888;font-size:22px;padding:18px;margin:0">served by Test-HttpServer.ps1 on 192.168.3.108</p>
</body></html>
'@
$bytes=[Text.Encoding]::UTF8.GetBytes($html)
$listener=[System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any,$Port)
$listener.Start()
Write-Host "HTTP 服务监听 0.0.0.0:$Port (Ctrl+C 停)。等待请求…"
$header="HTTP/1.1 200 OK`r`nContent-Type: text/html; charset=utf-8`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n"
$hbytes=[Text.Encoding]::ASCII.GetBytes($header)
while($true){
  try{
    $c=$listener.AcceptTcpClient()
    $remote=$c.Client.RemoteEndPoint.ToString()
    $ns=$c.GetStream()
    $ns.ReadTimeout=2000
    # 读掉请求(尽力,读到 header 结束或超时)
    $buf=New-Object byte[] 4096
    try{ Start-Sleep -Milliseconds 60; while($ns.DataAvailable){ [void]$ns.Read($buf,0,$buf.Length) } }catch{}
    $ns.Write($hbytes,0,$hbytes.Length)
    $ns.Write($bytes,0,$bytes.Length)
    $ns.Flush(); $ns.Close(); $c.Close()
    Write-Host ("[{0}] 已响应 {1}" -f (Get-Date -Format HH:mm:ss),$remote)
  }catch{ Write-Host "err: $($_.Exception.Message)" }
}
