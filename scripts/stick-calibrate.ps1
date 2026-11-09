# Калибровка стиков Xbox 360 через XInput (слот 0)
# Запуск: powershell -ExecutionPolicy Bypass -File stick-calibrate.ps1
Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public class XIc{
[StructLayout(LayoutKind.Sequential)]public struct ST{public uint pk;public ushort b;public byte lt,rt;public short lx,ly,rx,ry;}
[DllImport("xinput1_4.dll")]public static extern int XInputGetState(int i,out ST s);}
'@

function Read-Pad { $s = New-Object XIc+ST; if ([XIc]::XInputGetState(0,[ref]$s) -ne 0) { return $null }; $s }
function Bar($v){ $n=[int](($v+32768)/65535*40); ('#'*$n).PadRight(40,'.') }

if (-not (Read-Pad)) { Write-Host "Геймпад в слоте 0 не найден." -ForegroundColor Red; exit }

# --- Шаг 1: центр (стики не трогать)
Write-Host "`nШАГ 1. Отпустите оба стика и не трогайте. Замер 3 сек..." -ForegroundColor Cyan
Start-Sleep 1
$n=0;$sum=@{lx=0;ly=0;rx=0;ry=0};$dev=@{lx=0;ly=0;rx=0;ry=0}
$end=(Get-Date).AddSeconds(3)
while((Get-Date) -lt $end){ $s=Read-Pad; $n++
  foreach($k in 'lx','ly','rx','ry'){ $sum[$k]+=$s.$k; $dev[$k]=[Math]::Max($dev[$k],[Math]::Abs($s.$k)) }
  Start-Sleep -Milliseconds 20 }
$center=@{}; foreach($k in 'lx','ly','rx','ry'){ $center[$k]=[int]($sum[$k]/$n) }

# --- Шаг 2: диапазон (круги стиками)
Write-Host "ШАГ 2. 10 секунд крутите ОБА стика по краю до упора, по кругу." -ForegroundColor Cyan
$min=@{lx=0;ly=0;rx=0;ry=0};$max=@{lx=0;ly=0;rx=0;ry=0}
$end=(Get-Date).AddSeconds(10)
while((Get-Date) -lt $end){ $s=Read-Pad
  foreach($k in 'lx','ly','rx','ry'){ if($s.$k -lt $min[$k]){$min[$k]=$s.$k}; if($s.$k -gt $max[$k]){$max[$k]=$s.$k} }
  Write-Host -NoNewline ("`rL x[{0}] y[{1}]   R x[{2}] y[{3}]" -f (Bar $s.lx).Substring(0,12),(Bar $s.ly).Substring(0,12),(Bar $s.rx).Substring(0,12),(Bar $s.ry).Substring(0,12))
  Start-Sleep -Milliseconds 30 }

# --- Отчёт
Write-Host "`n`n=== РЕЗУЛЬТАТ (идеал: центр 0, края ±32767) ===" -ForegroundColor Green
foreach($k in 'lx','ly','rx','ry'){
  $name = @{lx='Левый X';ly='Левый Y';rx='Правый X';ry='Правый Y'}[$k]
  "{0,-9} центр={1,6}  разброс покоя=±{2,-5} мин={3,6} макс={4,6}" -f $name,$center[$k],$dev[$k],$min[$k],$max[$k]
}
$worst = ($dev.Values | Measure-Object -Maximum).Maximum
$off   = ($center.Values | % {[Math]::Abs($_)} | Measure-Object -Maximum).Maximum
$dz = [Math]::Ceiling(([Math]::Max($worst,$off)+500)/32767*100)
Write-Host "`nРекомендуемая мёртвая зона: $dz% (≈$([Math]::Ceiling($dz/100*32767)) ед.)" -ForegroundColor Yellow
if($off -gt 3000){ Write-Host "Центр сильно смещён (>3000) — это дрейф/износ стика, программная мёртвая зона обязательна." -ForegroundColor Yellow }
foreach($k in 'lx','ly','rx','ry'){ if($max[$k] -lt 30000 -or $min[$k] -gt -30000){ Write-Host "Ось $k не доходит до края — возможно, вы не довели стик до упора." -ForegroundColor Yellow } }
Write-Host "`nВ играх/Steam выставьте мёртвую зону не меньше рекомендованной."
