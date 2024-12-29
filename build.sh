#!/bin/bash
KERNEL_DIR=$(pwd)
CLANG="neutron"
TC_DIR="$HOME/toolchains/$CLANG-clang"

AK3_URL="https://github.com/loukious/AnyKernel3.git"
AK3_BRANCH="master"
AK3_DIR="$HOME/vayu/anykernel"

# Check if AK3 exist
if ! [ -d "$AK3_DIR" ]; then
	echo "$AK3_DIR not found! Cloning to $AK3_DIR..."
	if ! git clone -q --single-branch --depth 1 -b $AK3_BRANCH $AK3_URL $AK3_DIR; then
		echo "Cloning failed! Aborting..."
		exit 1
	fi
else
	echo "$AK3_DIR found! Update $AK3_DIR"
	cd $AK3_DIR
	git pull
	cd $KERNEL_DIR
fi

if ! [ -d "$TC_DIR" ]; then
	echo "$TC_DIR not found! Setting it up..."
	mkdir -p $TC_DIR
	cd $TC_DIR
	bash <(curl -s "https://raw.githubusercontent.com/Neutron-Toolchains/antman/main/antman") -S=10032024
	bash <(curl -s "https://raw.githubusercontent.com/Neutron-Toolchains/antman/main/antman") --patch=glibc
	cd $KERNEL_DIR
else
	echo "$TC_DIR found!"
fi

# Handle DEFCONFIG argument
if [ "$(echo "$1" | tr '[:upper:]' '[:lower:]')" == "nethunter" ]; then
    DEFCONFIG="nethunter_defconfig"
    ZIP_PREFIX="NetHunter"
elif [ "$(echo "$1" | tr '[:upper:]' '[:lower:]')" == "vayu" ]; then
    DEFCONFIG="vayu_user_defconfig"
    ZIP_PREFIX="Vayu"
else
    echo "Usage: $0 [nethunter|vayu] [version]"
    exit 1
fi

# Check if version argument is empty
if [ -z "$2" ]; then
	echo "Version argument is empty!"
	echo "Usage: $0 [nethunter|vayu] [version]"
	exit 1
fi

# Setup environment
SECONDS=0 # builtin bash timer
ZIPNAME="CrDroid-$ZIP_PREFIX-Loukious-$2-$(date '+%Y%m%d-%H%M').zip"
export PROC="-j$(nproc)"

echo "Building kernel with DEFCONFIG: $DEFCONFIG"

# Setup ccache environment
export USE_CCACHE=1
export CCACHE_EXEC=/usr/local/bin/ccache
CROSS_COMPILE+="ccache clang"

# Toolchain environtment
export PATH="$TC_DIR/bin:$PATH"
export THINLTO_CACHE_DIR="/tmp/thinlto-cache"
export KBUILD_COMPILER_STRING="$($TC_DIR/bin/clang --version | head -n 1 | perl -pe 's/\((?:http|git).*?\)//gs' | sed -e 's/  */ /g' -e 's/[[:space:]]*$//' -e 's/^.*clang/clang/')"
export STRIP="$TC_DIR/bin/$(echo "$(find "$TC_DIR/bin" -type f -name "aarch64-*-gcc")" | awk -F '/' '{print $NF}' | sed -e 's/gcc/strip/')"

# Kernel Details
KERNEL_VER="$(date '+%Y%m%d-%H%M')"
OUT="$HOME/vayu/kernel-out"
function clean_all {
	cd $KERNEL_DIR
	echo
	rm -rf prebuilt
	rm -rf out && rm -rf $OUT
}

clean_all
echo
echo "All Cleaned now."

# Delete old file before build
if [[ $1 = "-c" || $1 = "--clean" ]]; then
	rm -rf $OUT
fi

# Make out folder
mkdir -p $HOME/vayu/kernel-out
make $PROC O=$OUT ARCH=arm64 $DEFCONFIG \
	CLANG_PATH=$TC_DIR/bin \
	CC="ccache clang" \
	CXX="ccache clang++" \
	HOSTCC="ccache clang" \
	HOSTCXX="ccache clang++" \
	LD=ld.lld \
	AR=llvm-ar \
	AS=llvm-as \
	NM=llvm-nm \
	OBJCOPY=llvm-objcopy \
	OBJDUMP=llvm-objdump \
	STRIP=llvm-strip \
	CROSS_COMPILE="aarch64-linux-gnu-" \
	CROSS_COMPILE_COMPAT=arm-linux-gnueabi- \
	CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
	LDFLAGS="--thinlto-cache-dir=$THINLTO_CACHE_DIR"

# Regened defconfig
#  Test use ccache. -j$(nproc --all)
if [[ $1 == "-r" || $1 == "--regen" ]]; then
	cp $OUT/.config arch/arm64/configs/$DEFCONFIG
	echo -e "\nRegened defconfig succesfully!"
	exit 0
else
	echo -e "\nStarting compilation...\n"
	make $PROC O=$OUT ARCH=arm64 \
		CLANG_PATH=$TC_DIR/bin \
		CC="ccache clang" \
		CXX="ccache clang++" \
		HOSTCC="ccache clang" \
		HOSTCXX="ccache clang++" \
		LD=ld.lld \
		AR=llvm-ar \
		AS=llvm-as \
		NM=llvm-nm \
		OBJCOPY=llvm-objcopy \
		OBJDUMP=llvm-objdump \
		STRIP=llvm-strip \
		CROSS_COMPILE="aarch64-linux-gnu-" \
		CROSS_COMPILE_COMPAT=arm-linux-gnueabi- \
		CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
		LDFLAGS="--thinlto-cache-dir=$THINLTO_CACHE_DIR"
fi

# Creating zip flashable file
function create_zip {
	#Copy AK3 to out/Anykernel3
	cp -r $AK3_DIR AnyKernel3
	cp $OUT/arch/arm64/boot/Image.gz AnyKernel3
	cp $OUT/arch/arm64/boot/dtbo.img AnyKernel3

	# Change dir to AK3 to make zip kernel
	cd AnyKernel3
	zip -r9 "../$ZIPNAME" * -x '*.git*' README.md *placeholder

	#Back to out folder and clean
	cd ..
	rm -rf AnyKernel3
	rm -rf $OUT/arch/arm64/boot ##keep boot to compile rom
	echo -e "\nCompleted in $((SECONDS / 60)) minute(s) and $((SECONDS % 60)) second(s) !"
	echo "Zip: $ZIPNAME"
}


if [ -f "$OUT/arch/arm64/boot/Image.gz" ] && [ -f "$OUT/arch/arm64/boot/dtbo.img" ]; then
	echo -e "\nKernel compiled succesfully!\n"
	create_zip
	echo -e "\nDone !"
else
	echo -e "\nFailed!"
fi
