obj-m += nf10.o
nf10-objs += ael2005_conf.o
nf10-objs += nf10_main.o
nf10-objs += nf10_lbuf.o
nf10-objs += nf10_ethtool.o
nf10-objs += nf10_user.o
ifeq ($(CONFIG_XEN_NF_BACKEND),y)
nf10-objs += xen_nfback.o
CONFIG_LBUF_COHERENT=y
endif
ifeq ($(CONFIG_SKBPOOL),y)
nf10-objs += skbpool.o
endif

ifeq ($(NDEBUG),)
ccflags-y += -g -DDEBUG
endif

ccflags-$(CONFIG_XEN_NF_BACKEND) += -DCONFIG_XEN_NF_BACKEND
ccflags-$(CONFIG_SKBPOOL) += -DCONFIG_SKBPOOL
ccflags-$(CONFIG_PROFILE) += -DCONFIG_PROFILE
ccflags-$(CONFIG_LBUF_COHERENT) += -DCONFIG_LBUF_COHERENT

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
