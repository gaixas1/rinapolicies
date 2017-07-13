obj-m += rmt_eqta.o

ifndef IRATI_KSDIR
IRATI_KSDIR=/root/stack/kernel
endif

ccflags-y = -Wtype-limits -I${IRATI_KSDIR}

all:
	make -C /lib/modules/$(shell uname -r)/build KBUILD_EXTRA_SYMBOLS=${IRATI_KSDIR}/Module.symvers  M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean