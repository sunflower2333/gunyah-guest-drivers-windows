[CmdletBinding()]
param([string]$Xml = 'C:\DroidVM\ZinkD3D\etw\anchor.xml', [int]$First = 0, [int]$Last = 0)
$cur = New-Object System.Text.StringBuilder
$seq = New-Object System.Collections.Generic.List[string]
foreach ($l in [System.IO.File]::ReadLines($Xml)) {
    [void]$cur.Append($l).Append("`n")
    if ($l -match '</Event>') {
        $ev = $cur.ToString()
        $t = [regex]::Match($ev,'<Task>([^<]*)</Task>').Groups[1].Value.Trim()
        $o = [regex]::Match($ev,'<Opcode>([^<]*)</Opcode>').Groups[1].Value.Trim()
        $d = [regex]::Matches($ev,'<Data Name="([^"]+)">([^<]*)</Data>')
        $f = ($d | ForEach-Object { $_.Groups[1].Value + '=' + $_.Groups[2].Value.Trim() }) -join ' '
        # keep only call-sequence relevant tasks
        if ($t -in @('68','19','14','494','9','147','168')) { $seq.Add(("[{0}/{1}] {2}" -f $t,$o,$f)) }
        [void]$cur.Clear()
    }
}
Write-Output "KEPT=$($seq.Count)"
if ($First -gt 0) { $seq | Select-Object -First $First }
elseif ($Last -gt 0) { $seq | Select-Object -Last $Last }
else { $seq }
