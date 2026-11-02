// NDS-ARM7FIX: misma proteccion que el arreglo oficial de DSWiFi (commit 59d2636):
// si Wifi_Update() se llama otra vez mientras ya se esta ejecutando (desde la interrupcion de
// VBlank o desde el mensaje WIFI_SYNC de la FIFO), la segunda llamada sale sin hacer nada.
#include <stdbool.h>

void __real_Wifi_Update(void);

static volatile bool wifi_update_active = false;

void __wrap_Wifi_Update(void)
{
    if (wifi_update_active)
        return;
    wifi_update_active = true;
    __real_Wifi_Update();
    wifi_update_active = false;
}
