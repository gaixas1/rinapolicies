ifndef KREL
KREL=4.9.0-0.bpo.3-amd64
endif

ifndef KDIR
KDIR=/lib/modules/$(KREL)/build
endif

ifndef IRATI_KSDIR
IRATI_KSDIR=/root/stack/kernel
endif

ccflags-y = -Wtype-limits -I/root/stack/kernel

obj-m := rmt_be-plugin.o
rmt_be-plugin-y := rmt_be.o


all:
	$(MAKE) -C $(KDIR) KBUILD_EXTRA_SYMBOLS=${IRATI_KSDIR}/Module.symvers M=$$PWD

clean:
	rm -r -f *.o *.ko *.mod.c *.mod.o Module.symvers .*.cmd .tmp_versions modules.order

install:
	$(MAKE) -C $(KDIR) M=$$PWD modules_install
	cp rmt_be-plugin.manifest /lib/modules/$(KREL)/extra/
	depmod -a

uninstall:
	@echo "This target has not been implemented yet"
	@exit 1
