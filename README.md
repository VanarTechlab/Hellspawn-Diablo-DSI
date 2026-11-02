# Hellspawn – Diablo DSi

Port gratuito de **Diablo** para **Nintendo DSi / 3DS**, por **Vanar** ([VanarTechlab](https://www.youtube.com/@VanarTechlab)).
Basado en [DevilutionX](https://github.com/diasurgical/devilutionX). Web: <https://vanartechlab.pages.dev/hellspawn/>

> **Esta es una versión modificada de DevilutionX.** Ver [MODIFICACIONES.md](MODIFICACIONES.md).
> Proyecto no oficial, sin relación con Blizzard Entertainment.

## Descargar y jugar

La versión lista para jugar está en **[Releases](https://github.com/VanarTechlab/Hellspawn-Diablo-DSI/releases)**.
Es gratis y lo será siempre. **No incluye archivos de Blizzard**: necesitas tu propio `DIABDAT.MPQ`
de una copia original de Diablo (por ejemplo, la de GOG). El zip trae un programa para preparar la SD.

## Qué tiene

- Juego en la pantalla táctil (tocar = hacer clic) y HUD, inventario y menús en la de arriba.
- Multijugador local por NiFi entre consolas (DSi XL y New 3DS XL probados).
- Música por streaming desde la SD y cinemáticas convertidas a un formato propio (.dsv).
- Extras: Soundtrack, Cinemáticas, BestiarioDex, idioma Español/English, créditos DSi.

## Código

Rama `main` = DevilutionX + los cambios de Hellspawn. Los cambios en el motor están marcados con
`#ifdef __NDS__` y comentarios `/* NDS-XXX */`.

| Ruta | Qué es |
|---|---|
| `nds/sdl_nds.c` | Capa SDL 1.2 propia para la DSi: vídeo en dos pantallas, táctil, botones, archivos (libfat), hilos cooperativos, audio, HUD |
| `nds/fake-sdl-include/` | Cabeceras de SDL 1.2 (LGPL-2.1) usadas para compilar contra la capa propia |
| `Source/dvlnet/protocol_nds.*` | Red local NiFi (DSWiFi) con capa fiable propia |
| `Source/engine/sound_nds.inc` | Sonido |
| `nds/arm7/` | Núcleo ARM7 de BlocksDS con un arreglo para `Wifi_Update` |
| `nds/configurar.sh`, `nds/compilar.sh` | Scripts de compilación (Ubuntu/WSL) |
| `nds/herramientas/` | Empaquetado de la descarga, conversión de cinemáticas y medición |

## Compilar (Ubuntu / WSL)

Necesitas [BlocksDS](https://blocksds.skylyrac.net/) (Wonderful Toolchain, GCC 16) instalado en `/opt/wonderful`.

1. Compila `nds/sdl_nds.c` como librería `libSDL.a` (ver el principio de `nds/compilar.sh`).
2. `nds/configurar.sh` crea la carpeta `build-nds` con todas las opciones de CMake.
3. `nds/compilar.sh` compila el motor y crea el `.nds` con `ndstool`.

Los scripts usan rutas de mi equipo (`~/fake-sdl`, `~/sdl_nds_test`, `~/arm7_fix`): ajústalas a las tuyas.

## Licencias

- DevilutionX: **Sustainable Use License 1.0** ([LICENSE.md](LICENSE.md)): uso y reparto **gratis y sin fines comerciales**. Hellspawn mantiene esa licencia.
- Cabeceras de SDL 1.2: LGPL-2.1. Núcleo ARM7 de BlocksDS: Zlib. Lua: MIT.
- Diablo y sus datos son de Blizzard Entertainment y **no** están en este repositorio.

## Créditos

- **Vanar**: port a DSi.
- **Night Stroke**: imagen del menú principal e icono (no incluidos en el código).
- **Yagatsu18**: demotester.
- **DevilutionX Team**: el motor.
- **Blizzard North**: creadores de Diablo.

Las donaciones son voluntarias y no desbloquean nada: <https://vanar-techlab-shop.fourthwall.com>

---

*English:* Free, non-commercial port of Diablo (DevilutionX) to Nintendo DSi/3DS. Bring your own `DIABDAT.MPQ`.
This is a modified version of DevilutionX; see `MODIFICACIONES.md`. Not affiliated with Blizzard.
