.PHONY: all switch switch-p4 switch-ctrl clean

ifndef SDE
$(error SDE is not set. Export SDE and SDE_INSTALL before building)
endif

ifndef SDE_INSTALL
SDE_INSTALL := $(SDE)/install
endif

all: switch

switch: switch-p4 switch-ctrl

switch-p4: sail.p4
	$(SDE)/p4_build.sh $<

switch-ctrl: sail_ctrl

sail_ctrl: test_control/ctrl.c test_control/headers.h config.h
	gcc -I$$SDE_INSTALL/include -g -O2 -std=gnu11  -L/usr/local/lib -L$$SDE_INSTALL/lib \
		$< -o $@ \
		-ldriver -lbfsys -lbfutils -lbf_switchd_lib -lm -lpthread  \

clean:
	-rm -f sail_ctrl test bf_drivers.log* zlog-cfg-cur
