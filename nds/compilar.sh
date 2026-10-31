#!/bin/bash
# Compila la capa SDL, el motor y genera el .nds y el .elf con simbolos.
# Uso: nds/compilar.sh [carpeta]   (por defecto build-nds)
source /opt/wonderful/bin/wf-env
DIR="${1:-build-nds}"
TC=/opt/wonderful/toolchain/gcc-arm-none-eabi/bin
cp ~/sdl_nds_test/source/sdl_nds.c ~/devilutionX/nds/sdl_nds.c   # copia versionada
cd ~/fake-sdl && cp ~/sdl_nds_test/source/sdl_nds.c real/sdl_nds.c && rm -f real/sdl_nds.o && $TC/arm-none-eabi-gcc -mcpu=arm946e-s+nofp -mthumb -Os -ffunction-sections -fdata-sections -D__NDS__ -D__BLOCKSDS__ -DARM9 -I$HOME/fake-sdl/include -I$BLOCKSDS/libs/libnds/include -I$BLOCKSDS/libs/dswifi/include -c real/sdl_nds.c -o real/sdl_nds.o 2>&1 | grep -E 'error' ; rm -f lib/libSDL.a && $TC/arm-none-eabi-ar rcs lib/libSDL.a real/sdl_nds.o && echo "LIBRERIA OK" || exit 1
cd ~/devilutionX
if cmake --build "$DIR" -j $(nproc) > /tmp/build.log 2>&1; then
  echo "MOTOR OK"
else
  grep -n "error" -A 3 /tmp/build.log | head -40; exit 1
fi
$BLOCKSDS/tools/ndstool/ndstool -c ~/devilutionx_test.nds -7 /home/vanar/arm7_fix/arm7_dswifi_fix.elf -9 "$DIR/devilutionx.elf" -b ~/sdl_nds_test/icon.gif "DevilutionX;DSi;port" > /dev/null && echo "NDS OK"
cd "$DIR"
sed -e 's/ -s / /g' -e 's/-Wl,-s / /g' -e 's/-Wl,--strip-all / /g' -e 's/--strip-all / /g' -e 's#-o devilutionx.elf#-o devilutionx_sym.elf -Wl,-Map=devilutionx.map#' CMakeFiles/devilutionx.dir/link.txt > /tmp/link_sym.sh
bash /tmp/link_sym.sh 2>&1 | grep -E 'error|undefined' | head
ls -la --time-style=+%H:%M:%S ~/devilutionx_test.nds devilutionx_sym.elf ~/sdl_nds_test/source/sdl_nds.c
cd ~ && explorer.exe .
