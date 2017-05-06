setenv machid 1029
setenv bootm_boot_mode sec
setenv bootargs 'console=ttyS0,115200 noinitrd disp.screen0_output_mode=EDID:1280x720p50 init=/init root=/dev/mmcblk0p2 rootwait panic=10 ${extra}'
fatload mmc 0 0x43000000 script.bin 
fatload mmc 0 0x42000000 zImage 
bootz 0x42000000
