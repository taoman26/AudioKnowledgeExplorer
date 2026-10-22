#!/usr/bin/env bash
# Runs inside the builder container (see build-appimage.sh): builds the app for other machines and packs the AppImage.
#   /src  = the repository (read only)   /out = where the AppImage goes   /work = scratch
set -euo pipefail
SRC=/src OUT=/out WORK=/work
VERSION=${AKE_VERSION:-$(sed -n 's/^project(AudioKnowledgeExplorer VERSION \([0-9.]*\).*/\1/p' $SRC/CMakeLists.txt)}
export APPIMAGE_EXTRACT_AND_RUN=1          # no FUSE inside a container
export ARCH=x86_64

mkdir -p $WORK/tools && cd $WORK/tools
fetch() { [ -f "$2" ] || curl -fsSL -o "$2" "$1"; chmod +x "$2"; sha256sum "$2"; }
GH=https://github.com/linuxdeploy
fetch $GH/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage linuxdeploy
fetch $GH/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage linuxdeploy-plugin-qt
fetch $GH/linuxdeploy-plugin-gstreamer/raw/master/linuxdeploy-plugin-gstreamer.sh linuxdeploy-plugin-gstreamer.sh
export PATH=$WORK/tools:$PATH

echo "== build the app (portable: AVX2 / FMA / F16C, no -march=native)"
cmake -S $SRC -B $WORK/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAKE_PORTABLE=ON \
    ${AKE_WHISPER_DIR:+-DAKE_WHISPER_DIR=$AKE_WHISPER_DIR}
cmake --build $WORK/build --target AudioKnowledgeExplorer ake_cli

echo "== Qt6 input-method plugin for fcitx5 (Ubuntu 22.04 has none: no Japanese input without it)"
if [ ! -f $WORK/fcitx5-qt/build/qt6/platforminputcontext/libfcitx5platforminputcontextplugin.so ]; then
    rm -rf $WORK/fcitx5-qt && git clone -q --depth 1 --branch 5.0.14 https://github.com/fcitx/fcitx5-qt.git $WORK/fcitx5-qt
    cmake -S $WORK/fcitx5-qt -B $WORK/fcitx5-qt/build -G Ninja -DENABLE_QT4=OFF -DENABLE_QT5=OFF -DENABLE_QT6=ON \
        -DBUILD_ONLY_PLUGIN=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build $WORK/fcitx5-qt/build
fi

echo "== AppDir"
APPDIR=$WORK/AppDir
rm -rf $APPDIR && mkdir -p $APPDIR/usr/bin
install -m755 $WORK/build/AudioKnowledgeExplorer $WORK/build/ake_cli $APPDIR/usr/bin/
# MeCab dictionary (JUMAN, UTF-8): looked up next to the executable (MeCabTokenizer::create)
DIC=$(dirname "$(find /var/lib/mecab /usr/lib -name sys.dic -path '*juman*' 2>/dev/null | head -1)")
echo "dictionary: $DIC"
mkdir -p $APPDIR/usr/share/AudioKnowledgeExplorer/mecab/jumandic
cp -L $DIC/* $APPDIR/usr/share/AudioKnowledgeExplorer/mecab/jumandic/
mkdir -p $APPDIR/usr/share/doc/AudioKnowledgeExplorer
cp $SRC/LICENSE $APPDIR/usr/share/doc/AudioKnowledgeExplorer/LICENSE
cp /usr/share/doc/mecab-jumandic-utf8/copyright $APPDIR/usr/share/doc/AudioKnowledgeExplorer/mecab-jumandic.copyright 2>/dev/null || true

echo "== linuxdeploy"
export QMAKE=$(command -v qmake6)
export DEPLOY_PLATFORM_THEMES=1
export EXTRA_PLATFORM_PLUGINS="libqxcb.so"
export LINUXDEPLOY_OUTPUT_VERSION=$VERSION
mkdir -p $OUT
cd $WORK
linuxdeploy --appdir $APPDIR \
    --executable $APPDIR/usr/bin/AudioKnowledgeExplorer --executable $APPDIR/usr/bin/ake_cli \
    --desktop-file $SRC/packaging/appimage/AudioKnowledgeExplorer.desktop \
    --icon-file $SRC/packaging/appimage/audioknowledgeexplorer.svg --icon-filename audioknowledgeexplorer \
    --plugin qt --plugin gstreamer

# the database driver: the Qt plugin tool finds the Sql module but does not copy its plugin (Qt 6.2 from Ubuntu); the library it
# needs (libsqlite3) is then picked up by a second run
mkdir -p $APPDIR/usr/plugins/sqldrivers
cp $(qmake6 -query QT_INSTALL_PLUGINS)/sqldrivers/libqsqlite.so $APPDIR/usr/plugins/sqldrivers/
linuxdeploy --appdir $APPDIR --deploy-deps-only $APPDIR/usr/plugins/sqldrivers

# the fcitx5 plugin goes next to the Qt ones that were just copied
mkdir -p $APPDIR/usr/plugins/platforminputcontexts
cp $WORK/fcitx5-qt/build/qt6/platforminputcontext/libfcitx5platforminputcontextplugin.so $APPDIR/usr/plugins/platforminputcontexts/

# Qt6 needs libOpenGL.so.0 (libglvnd), which linuxdeploy leaves out as a "system" library, and a desktop without any Qt6 program
# may not have it. It is a small dispatch stub: only it is added (libGL / libGLdispatch / the drivers stay the host's).
cp -L /usr/lib/x86_64-linux-gnu/libOpenGL.so.0 $APPDIR/usr/lib/

# What is inside, with the Ubuntu package each library came from: the AppImage contains GPL parts (Ubuntu's FFmpeg, GStreamer
# "ugly"), so whoever gets it must be able to find the matching sources (apt source <package> on Ubuntu 22.04 / jammy).
DOC=$APPDIR/usr/share/doc/AudioKnowledgeExplorer
{
    echo "AudioKnowledgeExplorer $VERSION: bundled libraries and the Ubuntu 22.04 (jammy) packages they come from."
    echo "Sources: https://packages.ubuntu.com/source/jammy/<source package>, or: apt source <package>"
    echo "Other parts: whisper.cpp $(sed -n 's/^set(AKE_WHISPER_TAG "\([^"]*\)".*/\1/p' $SRC/CMakeLists.txt) (MIT), fcitx5-qt 5.0.14 (LGPL-2.1+/BSD), MeCab dictionary: mecab-jumandic-utf8 (BSD)."
    echo
    set +e +o pipefail                       # a library with no dpkg owner (our own) is simply skipped
    find $APPDIR/usr/lib $APPDIR/usr/plugins -type f \( -name '*.so' -o -name '*.so.*' \) -printf '%f\n' | sort -u | while read n; do
        src=$(find /usr/lib /lib -name "$n" 2>/dev/null | head -1)
        [ -n "$src" ] && dpkg -S "$(readlink -f "$src")" 2>/dev/null | head -1 | cut -d: -f1
    done | sort -u | while read pkg; do
        [ -n "$pkg" ] && echo "$pkg $(dpkg-query -W -f='${Version}' $pkg 2>/dev/null)"
    done
    set -eo pipefail
} > $DOC/BUNDLED-PACKAGES.txt
echo "bundled packages: $(grep -c ' ' $DOC/BUNDLED-PACKAGES.txt) lines"

linuxdeploy --appdir $APPDIR --output appimage
mv -f $WORK/AudioKnowledgeExplorer*.AppImage $OUT/
ls -la $OUT
