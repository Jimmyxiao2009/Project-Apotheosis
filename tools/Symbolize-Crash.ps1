# Symbolize-Crash.ps1 — 解析 minidump 的异常上下文,符号化出错 PC + 栈回溯(Harness.exe 帧)。
param([Parameter(Mandatory)][string]$Dump,
      [string]$Exe='E:\Apotheosis\harness\ARM\Release\Harness\Harness.exe',
      [int]$ImageBase=0x400000)
$ErrorActionPreference='Stop'
$b=[IO.File]::ReadAllBytes($Dump)
function U32($o){[BitConverter]::ToUInt32($b,$o)}
function U64($o){[BitConverter]::ToUInt64($b,$o)}
$nS=U32 8; $dir=U32 12
$streams=@{}
for($i=0;$i -lt $nS;$i++){ $o=$dir+$i*12; $streams[[int](U32 $o)]=@{Size=U32 ($o+4);Rva=U32 ($o+8)} }

# 模块表(type 4)
$mods=@()
if($streams.ContainsKey(4)){ $r=$streams[4].Rva; $nM=U32 $r
  for($m=0;$m -lt $nM;$m++){ $mo=$r+4+$m*108; $base=U64 $mo; $sz=U32 ($mo+8); $nr=U32 ($mo+20); $sl=U32 $nr
    $name=[Text.Encoding]::Unicode.GetString($b,$nr+4,$sl); $mods+=[pscustomobject]@{Name=[IO.Path]::GetFileName($name);Base=$base;End=$base+$sz} } }
$harness=$mods|Where-Object{$_.Name -match 'Harness\.exe'}|Select-Object -First 1

# 异常流(type 6)
$exCode=$null;$pc=$null;$lr=$null;$sp=$null
if($streams.ContainsKey(6)){ $r=$streams[6].Rva
  $exCode=U32 ($r+8)
  $ctxRva=U32 ($r+164)
  $pc=U32 ($ctxRva+64); $lr=U32 ($ctxRva+60); $sp=U32 ($ctxRva+56)
}
$codeNames=@{0x80000002='DATATYPE_MISALIGNMENT';0xC0000005='ACCESS_VIOLATION';0xC0000409='STACK_BUFFER_OVERRUN/__fastfail';0xC000001D='ILLEGAL_INSTRUCTION';0x80000003='BREAKPOINT';0xE06D7363='C++ EXCEPTION';0xC00000FD='STACK_OVERFLOW';0xC0000094='INT_DIVIDE_BY_ZERO'}
$cn=$codeNames[[uint32]$exCode]; if(-not $cn){$cn='?'}
Write-Host ("异常码: 0x{0:X8} {1}" -f $exCode,$cn) -ForegroundColor Yellow
Write-Host ("PC=0x{0:X8} LR=0x{1:X8} SP=0x{2:X8}" -f $pc,$lr,$sp)

$sym='C:\Program Files\LLVM\bin\llvm-symbolizer.exe'
function SymAddr($addr){
  if(-not $harness){return '(无 Harness 模块)'}
  if($addr -lt $harness.Base -or $addr -ge $harness.End){
    $m=$mods|Where-Object{$addr -ge $_.Base -and $addr -lt $_.End}|Select-Object -First 1
    if($m){return "$($m.Name)+0x$('{0:X}' -f ($addr-$m.Base))"} else {return '(不在模块内)'}
  }
  $rva=($addr -band 0xFFFFFFFE)-$harness.Base
  $line=(& $sym --obj=$Exe --demangle --functions=linkage ('0x{0:X}' -f ($ImageBase+$rva)) | Select-Object -First 1)
  return $line
}
Write-Host ("`nPC  -> {0}" -f (SymAddr $pc)) -ForegroundColor Cyan
Write-Host ("LR  -> {0}" -f (SymAddr $lr))

# 栈回溯:扫 SP 所在栈内存里落在 Harness.exe 的返回地址
if($streams.ContainsKey(3) -and $harness -and $sp){
  $ml=$streams[3].Rva; $nR=U32 $ml; $sRva=$null;$sStart=$null;$sSize=$null
  for($i=0;$i -lt $nR;$i++){ $o=$ml+4+$i*16; $st=U64 $o; $dz=U32 ($o+8); $dr=U32 ($o+12); if($sp -ge $st -and $sp -lt ($st+$dz)){$sRva=$dr;$sStart=$st;$sSize=$dz;break} }
  if($sRva){
    Write-Host "`n栈回溯(Harness.exe 帧,去重保序):" -ForegroundColor Cyan
    $seen=@{}; $cnt=0
    for($p=($sp-$sStart);$p -lt $sSize-3 -and $cnt -lt 24;$p+=4){
      $v=U32 ($sRva+$p)
      if($v -ge $harness.Base -and $v -lt $harness.End -and -not $seen.ContainsKey($v)){
        $seen[$v]=1; $cnt++
        Write-Host ("  {0}" -f (SymAddr $v))
      }
    }
  }
}
