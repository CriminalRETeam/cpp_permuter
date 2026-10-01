#!/bin/sh
# Compiles one gta2_re translation unit with VC6 under wine, using the same
# flags as gta2_lib in gta2_re's CMakeLists.txt / build.py --single_cpp.
#
#   compile.sh <src.cpp> <out.obj>
#
# GTA2_RE must point at a gta2_re checkout with the 3rdParty/gta2_re_compile_tools
# submodule. EXTRA_CFLAGS adds per-file flags (cmake/vc6.cmake), e.g. "/Gz" for
# Network_20324.cpp.
#
# Use it as: cpp_permuter -c 'examples/gta2/compile.sh {src} {obj}' ...
set -e
SRC=$1
OBJ=$2
: "${GTA2_RE:?set GTA2_RE to your gta2_re checkout}"
TOOLS="$GTA2_RE/3rdParty/gta2_re_compile_tools"

winpath() { printf 'Z:%s' "$(realpath -m "$1")" | tr / '\\'; }

export WINEDEBUG=-all
export WINEPATH="$(winpath "$TOOLS/VC98/Bin");$(winpath "$TOOLS/Common/MSDev98/Bin")"
export INCLUDE="$(winpath "$TOOLS/VC98/ATL/Include");$(winpath "$TOOLS/VC98/Include");$(winpath "$TOOLS/VC98/MFC/Include")"
export LIB="$(winpath "$TOOLS/VC98/Lib")"

# cl.exe exits 0 on warnings and 2 on errors; its output goes to stdout.
wine cl.exe /nologo /TP /c \
    /I"$(winpath "$GTA2_RE/Source")" /I"$(winpath "$GTA2_RE")" /I"$(winpath "$TOOLS")" \
    /DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS /D_CRT_NON_CONFORMING_SWPRINTFS /DIMGUI_DLL \
    /W3 /Zm1000 /EHsc /GX /ML /O2 /DNDEBUG $EXTRA_CFLAGS \
    /Fo"$(winpath "$OBJ")" "$(winpath "$SRC")"
