#!/bin/sh
# setup.sh [workdir]: a simulated handheld on an x86-64 PC. ROCKNIX's own DraStic (Linux aarch64, r2.5.2.2) runs under
# qemu-aarch64 user mode, against an arm64 Ubuntu 24.04 sysroot, drawing into an X display. QEMU is built from source
# with plugin support for the fnprof instruction profiler. See README.md here.
#
# Needs: Debian/Ubuntu host, network to github.com and ports.ubuntu.com, and these packages: clang lld llvm git
# ninja-build python3-venv libglib2.0-dev libpixman-1-dev pkg-config gcc make (and xvfb for a display).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:-$HERE/work}
mkdir -p "$W"; cd "$W"

# ---- arm64 sysroot: glibc, SDL2 and everything SDL2 loads ----
PKGS="libc6 libgcc-s1 libstdc++6 zlib1g libasound2t64 libsdl2-2.0-0 libpulse0 libsamplerate0 libx11-6 libxext6
      libxcursor1 libxi6 libxfixes3 libxrandr2 libxss1 libgbm1 libwayland-egl1 libwayland-client0 libwayland-cursor0
      libwayland-server0 libxkbcommon0 libdecor-0-0 libdbus-1-3 libsndfile1 libx11-xcb1 libxcb1 libsystemd0
      libasyncns0 libapparmor1 libxrender1 libxau6 libxdmcp6 libbsd0 libmd0 libflac12t64 libvorbis0a libvorbisenc2
      libopus0 libogg0 libmpg123-0t64 libmp3lame0 libcap2 liblz4-1 liblzma5 libzstd1 libgcrypt20 libgpg-error0
      libffi8 libexpat1 libdrm2 libegl1 libglvnd0 libgles2 libgl1 libglx0 libc6-dev linux-libc-dev libgcc-13-dev"
if [ ! -f sysroot/.done ]; then
    A=$W/apt; mkdir -p $A/etc/apt/apt.conf.d $A/etc/apt/preferences.d $A/var/lib/apt/lists/partial \
        $A/var/cache/apt/archives/partial $A/var/lib/dpkg debs
    touch $A/var/lib/dpkg/status
    for s in noble noble-updates; do
        echo "deb [arch=arm64 signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] http://ports.ubuntu.com/ubuntu-ports $s main universe"
    done > $A/etc/apt/sources.list
    printf 'Dir "%s/";\nDir::State "%s/var/lib/apt/";\nDir::State::status "%s/var/lib/dpkg/status";\nDir::Cache "%s/var/cache/apt/";\nDir::Etc "%s/etc/apt/";\nAPT::Architecture "arm64";\nAPT::Architectures { "arm64"; };\nAcquire::Languages "none";\n' \
        $A $A $A $A $A > $A/apt.conf
    APT_CONFIG=$A/apt.conf apt-get -qq update
    (cd debs && APT_CONFIG=$A/apt.conf apt-get -qq download $PKGS)
    rm -rf sysroot && mkdir sysroot
    for d in debs/*.deb; do dpkg-deb -x "$d" sysroot; done
    ln -sfn usr/lib sysroot/lib
    touch sysroot/.done
fi

# ---- DraStic as ROCKNIX installs it (ROCKNIX/packages drastic.tar.gz) and the RG DS's drastic.cfg ----
if [ ! -x drastic/drastic ]; then
    rm -rf rpk && git clone -q --depth 1 --filter=blob:none --no-checkout https://github.com/ROCKNIX/packages.git rpk
    git -C rpk checkout -q HEAD -- drastic.tar.gz
    rm -rf dtmp && mkdir dtmp && tar xzf rpk/drastic.tar.gz -C dtmp && mv dtmp/drastic/drastic_aarch64 drastic && rm -rf dtmp
    rm -rf rdist && git clone -q --depth 1 --filter=blob:none --sparse https://github.com/ROCKNIX/distribution.git rdist
    git -C rdist sparse-checkout set projects/ROCKNIX/packages/emulators/standalone/drastic-sa/config/RK3566
    mkdir -p drastic/config
    cp rdist/projects/ROCKNIX/packages/emulators/standalone/drastic-sa/config/RK3566/drastic.cfg.rgds drastic/config/drastic.cfg
fi
md5sum drastic/drastic

# ---- qemu-aarch64 with plugins, and the fnprof plugin ----
if [ ! -x qemu-build/qemu-aarch64 ]; then
    [ -d qemu-src ] || git clone -q --depth 1 -b v9.2.0 https://github.com/qemu/qemu.git qemu-src
    mkdir -p qemu-build && (cd qemu-build && ../qemu-src/configure --target-list=aarch64-linux-user --enable-plugins \
        --disable-docs --disable-werror >/dev/null && make -j"$(nproc)" qemu-aarch64 >/dev/null)
fi
python3 "$HERE/mkplugin.py" qemu-src fnprof.c
gcc -O2 -shared -fPIC -Iqemu-src/include/qemu $(pkg-config --cflags glib-2.0) -o fnprof.so fnprof.c $(pkg-config --libs glib-2.0)

# ---- the simulator shim ----
clang --target=aarch64-linux-gnu --sysroot="$W/sysroot" -fuse-ld=lld -O1 -shared -fPIC -o simshim.so "$HERE/simshim.c"
echo "simulator ready in $W: $HERE/run.sh <rom.nds> [seconds] [profile.txt]"
