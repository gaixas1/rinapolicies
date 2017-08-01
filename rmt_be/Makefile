ifndef IRATI_KSDIR
IRATI_KSDIR=/root/stack/kernel
endif

ccflags-y = -Wtype-limits -I${IRATI_KSDIR}

obj-m := rmt-be-plugin.o
rmt-be-plugin-y := rmt-be.o

all:
	make -C /lib/modules/$(shell uname -r)/build KBUILD_EXTRA_SYMBOLS=${IRATI_KSDIR}/Module.symvers M=$(PWD) modules
	
clean:
	rm -r -f *.o *.ko *.rc *.mod.c *.mod.o Module.symvers .*.cmd .tmp_versions modules.order

install:
	make -C /lib/modules/$(shell uname -r)/build M=$$PWD modules_install
	cp rmt-be-plugin.manifest /lib/modules/$(shell uname -r)/extra/
	depmod -a

uninstall:
	@echo "This target has not been implemented yet"
	@exit 1
	
	
	
	
	
	
	
	
	
	