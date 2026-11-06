sysuse auto, clear
foreach lit in "5.5.5" "0x10" "5foo" "1e" "1.2.3" {
  capture gen zzz = `lit'
  display "LITERAL=`lit' RC=" _rc
  capture drop zzz
}
