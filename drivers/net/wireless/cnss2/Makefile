# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_CNSS2) += cnss2.o

ccflags-y += -I$(srctree)/drivers/net/wireless/cnss_utils/
cnss2-y := main.o
cnss2-y += bus.o
cnss2-y += debug.o
cnss2-y += pci.o
cnss2-y += power.o
cnss2-y += genl.o
cnss2-$(CONFIG_CNSS2_QMI) += qmi.o coexistence_service_v01.o ip_multimedia_subsystem_private_service_v01.o