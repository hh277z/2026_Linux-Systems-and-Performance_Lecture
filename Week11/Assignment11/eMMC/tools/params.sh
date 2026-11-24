#!/usr/bin/env bash
# params.sh: shared experiment parameters. Usage: source tools/params.sh
# These shared values make measurements directly comparable.
# Level 1 uses QLC timings and a DDR52 host bus to contrast with the TLC/HS400 default.
L1_NAND=QLC
L1_TIMES="t_r_us=150 t_prog_us=1500 t_bers_us=4000"
L1_BUS="host_mbps=104"
L1_BUSNAME=DDR52

# Level 2 log workload and Level 3 UI/OTA workload.
L2_LINE=200
L2_ERR=100
L2_DELAY=100
L3_N=200
L3_PERIOD=500
L3_OTA=256
export L1_NAND L1_TIMES L1_BUS L1_BUSNAME L2_LINE L2_ERR L2_DELAY L3_N L3_PERIOD L3_OTA
echo "공통 실험 설정: L1 $L1_NAND ($L1_TIMES), $L1_BUSNAME ($L1_BUS) | "\
"L2 line ${L2_LINE} B, error 1/${L2_ERR}, max_delay ${L2_DELAY} ms | "\
"L3 burst ${L3_N} reads / ${L3_PERIOD} ms, OTA ${L3_OTA} MB"
