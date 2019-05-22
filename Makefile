#################################################
# Course: CMPT 433 Embedded Systems (Fall 2017) #
# Instructor: Brian Fraser                      #
#                                               #
# Authors: Karan Sharma, Mykhaylo Chavarha      #
# IDs: 301238119, 301309675                     #
# Emails: ksa95@sfu.ca, mchavarh@sfu.ca         #
#################################################

# with some settings from Robert Nelson's BBB kernel build script
# if KERNELRELEASE is defined, we've been invoked from the
# kernel build system and can use its language.
ifneq (${KERNELRELEASE},)
	obj-m := morsecode.o
	# Otherwise we were called directly from the command line.
	# Invoke the kernel build system.
else
	KERNEL_SOURCE := ${HOME}/cmpt433/work/bb-kernel/KERNEL/
	PWD := $(shell pwd)
	# Linux kernel 4.4 (which has cape manager support)
	CC=${HOME}/cmpt433/work/bb-kernel/dl/gcc-linaro-5.4.1-2017.05-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-
	BUILD=bone20
	CORES=4
	image=zImage
	PUBLIC_DRIVER_PWD=~/cmpt433/public/drivers
default:
	# Trigger kernel build for this module
	${MAKE} -C ${KERNEL_SOURCE} SUBDIRS=${PWD} -j${CORES} ARCH=arm \
	LOCALVERSION=-${BUILD} CROSS_COMPILE=${CC} ${address} \
	${image} modules
	# copy result to public folder
	cp *.ko ${PUBLIC_DRIVER_PWD}
clean:
	${MAKE} -C ${KERNEL_SOURCE} SUBDIRS=${PWD} clean
endif