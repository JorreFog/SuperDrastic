#!/bin/sh
# Builds an arm64 Ubuntu 24.04 sysroot from ports.ubuntu.com (adapted from SuperDrastic/tools/sim/setup.sh)
set -e
W=${1:?workdir}; cd "$W"
PKGS="libc6 libgcc-s1 libstdc++6 zlib1g libasound2t64 libsdl2-2.0-0 libpulse0 libsamplerate0 libx11-6 libxext6
      libxcursor1 libxi6 libxfixes3 libxrandr2 libxss1 libgbm1 libwayland-egl1 libwayland-client0 libwayland-cursor0
      libwayland-server0 libxkbcommon0 libdecor-0-0 libdbus-1-3 libsndfile1 libx11-xcb1 libxcb1 libsystemd0
      libasyncns0 libapparmor1 libxrender1 libxau6 libxdmcp6 libbsd0 libmd0 libflac12t64 libvorbis0a libvorbisenc2
      libopus0 libogg0 libmpg123-0t64 libmp3lame0 libcap2 liblz4-1 liblzma5 libzstd1 libgcrypt20 libgpg-error0
      libffi8 libexpat1 libdrm2 libegl1 libglvnd0 libgles2 libgl1 libglx0 libc6-dev linux-libc-dev libgcc-13-dev"
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
