# Núcleo ARM7

- `main.c`: núcleo ARM7 por defecto de BlocksDS v1.24.0 (`sys/arm7/main_core/source/main.c`, variante
  con DSWiFi), licencia Zlib, con `Wifi_Update()` en la interrupción de VBlank.
- `wifi_fix.c`: evita que `Wifi_Update()` se ejecute dos veces a la vez (mismo arreglo que el oficial
  de DSWiFi, BlocksDS issue 401). Se enlaza con `-Wl,--wrap=Wifi_Update`.

Resultado: `arm7_dswifi_fix.elf`, que `nds/compilar.sh` pasa a `ndstool` con `-7`.
