#!/bin/bash
# Crea una carpeta de compilacion para el port de DSi desde cero, con todas las opciones.
# Uso: nds/configurar.sh [carpeta]   (por defecto build-nds; si existe, se renombra a .viejo.FECHA)
set -e
source /opt/wonderful/bin/wf-env
cd "$HOME/devilutionX"
DIR="${1:-build-nds}"
if [ -d "$DIR" ]; then
  mv "$DIR" "$DIR.viejo.$(date +%Y%m%d_%H%M%S)"
  echo "La carpeta anterior se renombro (no se borra nada)."
fi
B=/opt/wonderful/thirdparty/blocksds/core
cmake -S . -B "$DIR" \
  -DCMAKE_TOOLCHAIN_FILE=$B/cmake/BlocksDSi.cmake \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_MODULE_PATH="$HOME/devilutionX/CMake/platforms/threads-stub" \
  -DNDS_ARCH_THUMB=ON \
  -DUSE_SDL1=ON -DNOSOUND=ON -DNONET=OFF -DDISABLE_TCP=ON -DDISABLE_ZERO_TIER=ON -DPACKET_ENCRYPTION=OFF \
  -DUNPACKED_MPQS=ON -DUNPACKED_SAVES=ON -DDISABLE_DEMOMODE=ON -DBUILD_TESTING=OFF \
  -DDEVILUTIONX_DISABLE_EXCEPTIONS=ON -DDEVILUTIONX_DISABLE_RTTI=ON \
  -DSDL1_VIDEO_MODE_BPP=8 "-DSDL1_VIDEO_MODE_FLAGS=SDL_HWSURFACE|SDL_FULLSCREEN" \
  -DHAS_KBCTRL=1 \
  -DKBCTRL_BUTTON_DPAD_LEFT=SDLK_LEFT -DKBCTRL_BUTTON_DPAD_RIGHT=SDLK_RIGHT \
  -DKBCTRL_BUTTON_DPAD_UP=SDLK_UP -DKBCTRL_BUTTON_DPAD_DOWN=SDLK_DOWN \
  -DKBCTRL_BUTTON_A=SDLK_LALT -DKBCTRL_BUTTON_B=SDLK_LCTRL \
  -DKBCTRL_BUTTON_X=SDLK_LSHIFT -DKBCTRL_BUTTON_Y=SDLK_SPACE \
  -DKBCTRL_BUTTON_LEFTSHOULDER=SDLK_TAB -DKBCTRL_BUTTON_RIGHTSHOULDER=SDLK_BACKSPACE \
  -DKBCTRL_BUTTON_START=SDLK_RETURN -DKBCTRL_BUTTON_BACK=SDLK_ESCAPE \
  -DKBCTRL_BUTTON_LEFTSTICK=SDLK_END \
  -DSDL_INCLUDE_DIR="$HOME/fake-sdl/include/SDL" \
  -DSDL_LIBRARY="$HOME/fake-sdl/lib/libSDL.a"
# Lua con enteros y reales de 32 bits (el DSi no tiene coma flotante de 64 bits rapida)
F=$(find "$DIR/_deps" -name luaconf.h | head -1)
if [ -z "$F" ]; then echo "AVISO: no encontre luaconf.h"; exit 1; fi
sed -i -E 's/^#define LUA_32BITS[[:space:]]+0/#define LUA_32BITS\t1/' "$F"
grep -n "define LUA_32BITS" "$F"
echo "CONFIGURACION OK en $DIR"
