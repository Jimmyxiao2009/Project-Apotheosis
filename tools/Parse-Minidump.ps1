# Parse-Minidump.ps1 — 直接解析 minidump,抠异常码/地址/出错模块(绕开坏掉的老 cdb)。
param([Parameter(Mandatory)][string]$Path)
$ErrorActionPreference='Stop'
$b=[IO.File]::ReadAllBytes($Path)
function U32($o){ [BitConverter]::ToUInt32($b,$o) }
function U64($o){ [BitConverter]::ToUInt64($b,$o) }

if ((U32 0) -ne 0x504D444D) { throw "不是 minidump(签名不符)" }
$nStreams = U32 8
$dirRva   = U32 12
Write-Host ("minidump: {0} 个流" -f $nStreams)

$streams=@{}
for($i=0;$i -lt $nStreams;$i++){
  $o=$dirRva + $i*12
  $type=[int](U32 $o); $size=U32 ($o+4); $rva=U32 ($o+8)
  $streams[$type]=@{Size=$size;Rva=$rva}
}

# --- ExceptionStream (type 6) ---
$exCode=$null;$exAddr=$null;$exFlags=$null;$nParam=0;$params=@()
if($streams.ContainsKey(6)){
  $r=$streams[6].Rva
  $exCode = U32 ($r+8)
  $exFlags= U32 ($r+12)
  $exAddr = U64 ($r+24)
  $nParam = U32 ($r+32)
  for($k=0;$k -lt [Math]::Min($nParam,15);$k++){ $params += (U64 ($r+40+$k*8)) }
} else { Write-Host "没有 ExceptionStream(可能不是异常崩溃)" -ForegroundColor Yellow }

# --- ModuleListStream (type 4) ---
$modules=@()
if($streams.ContainsKey(4)){
  $r=$streams[4].Rva
  $nMod=U32 $r
  for($m=0;$m -lt $nMod;$m++){
    $mo=$r+4+$m*108
    $base=U64 $mo; $isize=U32 ($mo+8); $nameRva=U32 ($mo+20)
    $slen=U32 $nameRva
    $name=[Text.Encoding]::Unicode.GetString($b,$nameRva+4,$slen)
    $modules += [pscustomobject]@{Name=[IO.Path]::GetFileName($name);Base=$base;Size=$isize;End=$base+$isize}
  }
}

$codeNames=@{
  0xC0000005='ACCESS_VIOLATION（访问违例,空指针/野指针）'
  0xC0000409='STACK_BUFFER_OVERRUN / __fastfail（RELEASE_ASSERT、/GS、__fastfail 主动终止）'
  0xC0000374='HEAP_CORRUPTION（堆损坏）'
  0xC0000135='DLL_NOT_FOUND（缺 DLL,打包/依赖缺失）'
  0xC0000139='ENTRYPOINT_NOT_FOUND（DLL 导出不匹配）'
  0xC06D007E='MODULE_NOT_FOUND（延迟加载模块缺失）'
  0xC06D007F='PROCEDURE_NOT_FOUND（延迟加载函数缺失）'
  0x80000003='BREAKPOINT（int3/DebugBreak/ASSERT）'
  0xE06D7363='C++ EXCEPTION（未捕获的 C++ 异常 throw）'
  0xC0000602='FAIL_FAST_EXCEPTION（__fastfail 不可继续）'
  0xC0000417='INVALID_CRT_PARAMETER（CRT 参数非法）'
}
Write-Host ""
Write-Host "================ 崩溃定性 ================" -ForegroundColor Cyan
if($null -ne $exCode){
  $hex='0x{0:X8}' -f $exCode
  $name=$codeNames[[uint32]$exCode]; if(-not $name){$name='(未知异常码)'}
  Write-Host ("异常码 : {0}  {1}" -f $hex,$name) -ForegroundColor Yellow
  Write-Host ("异常地址: 0x{0:X}" -f $exAddr)
  if($exCode -eq 0xE06D7363 -and $params.Count -ge 1){ Write-Host ("  C++ 异常,参数: {0}" -f (($params|ForEach-Object{'0x{0:X}' -f $_}) -join ', ')) }
  $fault=$modules | Where-Object { $exAddr -ge $_.Base -and $exAddr -lt $_.End } | Select-Object -First 1
  if($fault){
    $off=$exAddr-$fault.Base
    Write-Host ("出错模块: {0}  (基址 0x{1:X}, +0x{2:X})" -f $fault.Name,$fault.Base,$off) -ForegroundColor Green
  } else {
    Write-Host "出错地址不在任何已加载模块内(可能 JIT/动态代码或栈)" -ForegroundColor Magenta
  }
}
Write-Host ""
Write-Host ("已加载模块({0}):" -f $modules.Count) -ForegroundColor Cyan
$modules | Sort-Object Name | ForEach-Object { "  {0,-28} base=0x{1:X}  size=0x{2:X}" -f $_.Name,$_.Base,$_.Size }
