#!/bin/bash

WORKDIR="$(pwd)"
KERNEL_DEFAULE_CONFIG=unlasting_rb5_defconfig
KERNEL_DTB=qrb5165-rb5.dtb
JOBS="8"

function log_err() {
	echo -e "\e[31m $1 \e[0m"
	return 0
}

function log_info() {
	echo -e "\e[32m $1 \e[0m"
	return 0
}

export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
mkdir -p build deploy/modules
chmod 777 build
chmod 777 deploy
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=build ${KERNEL_DEFAULE_CONFIG}
# make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=build Image -j${JOBS}
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=build Image.gz -j${JOBS}
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=build modules -j${JOBS}
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=build qcom/${KERNEL_DTB}
# cp -v build/arch/arm64/boot/Image deploy/
cp -v build/arch/arm64/boot/Image.gz deploy/
cp -v build/arch/arm64/boot/dts/qcom/${KERNEL_DTB} deploy/
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=build modules_install INSTALL_MOD_PATH="${WORKDIR}/deploy/modules" INSTALL_MOD_STRIP=1
tar --xform s:'^./':: -czf deploy/kmods.tar.gz -C "${WORKDIR}/deploy/modules" .
cd "${WORKDIR}"

#cat ./Image.gz ./qrb5165-rb5.dtb > Image.gz+dtb
#mkbootimg --kernel Image.gz+dtb \
#              --ramdisk initrd.img \
#              --output boot-rb5.img \
#              --pagesize 4096 \
#              --base 0x80000000 \
#              --cmdline "root=PARTLABEL=rootfs console=tty0 console=ttyMSM0,1500000n8 pcie_pme=nomsi"

