export CROSS_COMPILE=/opt/gcc-linaro-6.3.1-2017.02-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-

$(CROSS_COMPILE)gcc -static am_sc2_dmx_test.c am_inject.c am_dmx.c linux_dvb.c -o am_sc2_dmx_test -lpthread -o am_sc2_dmx_test
#gcc am_sc2_dmx_test.c am_inject.c am_dmx.c linux_dvb.c -o am_sc2_dmx_test 

