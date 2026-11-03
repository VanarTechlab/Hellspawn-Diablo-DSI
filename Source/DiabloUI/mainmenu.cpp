#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "DiabloUI/diabloui.h"
#include "DiabloUI/selok.h" /* NDS-JUEGO */
#include "DiabloUI/ui_flags.hpp"
#include "DiabloUI/ui_item.h"
#include "engine/assets.hpp"
#include "engine/load_clx.hpp"
#include "engine/point.hpp"
#include "game_mode.hpp"
#include "utils/language.h"
#include "utils/ui_fwd.h"
#ifdef __NDS__ /* NDS-CREDITOS */
#include "controls/input.h"
#include "controls/menu_controls.h"
#include "utils/algorithm/container.hpp"
#include "movie.h" /* NDS-MENU */
#include "engine/sound.h" /* NDS-MENU2: music_start */
#include "utils/str_cat.hpp"
#include <algorithm> /* NDS-BESTIARIO */
#include <cstring>
#include "options.h"
#include <array>
#include <cstdio>
#include <string>
#include "engine/load_file.hpp"
#include "engine/palette.h"
#include "engine/render/clx_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/surface.hpp"
#include "tables/monstdat.h"
#include "utils/sdl_compat.h"
#endif

namespace devilution {
namespace {
int mainmenu_attract_time_out; // seconds
uint32_t dwAttractTicks;

std::vector<std::unique_ptr<UiItemBase>> vecMainMenuDialog;
std::vector<std::unique_ptr<UiListItem>> vecMenuItems;

_mainmenu_selections MainMenuResult;

#ifdef __NDS__ /* NDS-IDIOMA: textos propios en espanol o ingles segun el idioma del juego */
extern "C" void nds_set_lang_es(int on);
bool NdsEs()
{
	return GetLanguageCode() == "es";
}
#define NDS_T(es, en) (NdsEs() ? (es) : (en))
constexpr int NdsLangValue = 106;
constexpr int NdsLangEn = 140;
constexpr int NdsLangEs = 141;
constexpr int NdsOptionsValue = 107; /* NDS-OPCIONES */
constexpr int NdsScreensValue = 108;
constexpr int NdsScrBottom = 150;
constexpr int NdsScrTop = 151;
constexpr int NdsGameValue = 109; /* NDS-JUEGO */
constexpr int NdsGameDiablo = 160;
constexpr int NdsGameHellfire = 161;
extern "C" int nds_get_swap(void);
extern "C" void nds_set_swap(int on);
void NdsSetLanguage(const char *code)
{
	auto &opt = GetOptions().Language.code;
	opt = std::string_view(code);
	opt.SetActiveListIndex(opt.GetActiveListIndex()); /* aplica el cambio como el menu de opciones */
	SaveOptions();
	nds_set_lang_es(NdsEs() ? 1 : 0);
}
#endif

#ifdef __NDS__ /* NDS-CREDITOS: pantalla de agradecimientos de la version DSi */
constexpr int NdsCreditsValue = MAINMENU_SETTINGS; /* Settings no se muestra en la DSi: se reutiliza su valor */

void NdsShowDsiCredits()
{
	std::vector<std::unique_ptr<UiItemBase>> items;
	LoadBackgroundArt("ui_art\\credits");
	UiAddBackground(&items);
	const Point ui = GetUIRectangle().position;
	struct Line {
		const char *text;
		int y;
		UiFlags flags;
	};
	const Line lines[] = {
		/* NDS-CREDITOS3 */
		{ "Hellspawn - Diablo DSI", 5, UiFlags::FontSize42 | UiFlags::ColorUiGold },
		{ NDS_T("Agradecimiento especial a:", "Special thanks to:"), 55, UiFlags::FontSize30 | UiFlags::ColorUiSilver },
		{ "Night Stroke", 95, UiFlags::FontSize42 | UiFlags::ColorUiGold },
		{ NDS_T("Assets menu e icono", "Menu assets and icon"), 137, UiFlags::FontSize30 | UiFlags::ColorUiSilver },
		{ "Yagatsu18", 179, UiFlags::FontSize42 | UiFlags::ColorUiGold },
		{ "Demotester", 221, UiFlags::FontSize30 | UiFlags::ColorUiSilver },
		{ "DevilutionX Team", 263, UiFlags::FontSize42 | UiFlags::ColorUiGold },
		{ NDS_T("Codigo del Port", "Port code"), 305, UiFlags::FontSize30 | UiFlags::ColorUiSilver },
		{ "Blizzard North", 347, UiFlags::FontSize42 | UiFlags::ColorUiGold },
		{ NDS_T("Creadores de Diablo", "Creators of Diablo"), 389, UiFlags::FontSize30 | UiFlags::ColorUiSilver },
		{ NDS_T("Pulsa un boton para volver", "Press a button to go back"), 450, UiFlags::FontSize12 | UiFlags::ColorUiSilverDark },
	};
	for (const Line &l : lines) {
		const SDL_Rect rect = { (Sint16)(ui.x), (Sint16)(ui.y + l.y), 640, 50 };
		items.push_back(std::make_unique<UiArtText>(l.text, rect, l.flags | UiFlags::AlignCenter));
	}
	bool done = false;
	SDL_Event event;
	while (!done) {
		UiClearScreen();
		UiRenderItems(items);
		UiFadeIn();
		while (PollEvent(&event)) {
			if (c_any_of(GetMenuActions(event), [](MenuAction a) { return a != MenuAction_NONE; })) {
				done = true;
				break;
			}
			switch (event.type) {
			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
				done = true;
				break;
			}
			UiHandleEvents(&event);
		}
	}
	items.clear();
	ArtBackground = std::nullopt;
}
#endif

#ifdef __NDS__ /* NDS-MENU: menu principal con Extras y Cinematicas */
extern "C" int nds_cine_seen(const char *base);
extern "C" void nds_ui_mainmenu(int on); /* NDS-V12 */
int NdsMenuMode = 0; /* 0 = principal, 1 = Extras, 2 = Cinematicas */
constexpr int NdsExtrasValue = 100;
constexpr int NdsCineMenuValue = 101;
constexpr int NdsBackValue = 102;
constexpr int NdsLockedValue = 103;
constexpr int NdsCineBase = 110;
const char *const NdsCineNames[4] = { "Intro", "El Monasterio", "Diablo Derrotado", "El Encapuchado" }; /* NDS-MENU2 */
constexpr int NdsSoundValue = 104;
constexpr int NdsTrackBase = 130;
const _music_id NdsTracks[6] = { TMUSIC_TOWN, TMUSIC_CATHEDRAL, TMUSIC_CATACOMBS, TMUSIC_CAVES, TMUSIC_HELL, TMUSIC_INTRO };
const char *const NdsTrackNames[6] = { "Tristram", "Catedral", "Catacumbas", "Cuevas", "Infierno", "Tema Principal" };
const char *const NdsTrackNamesEn[6] = { "Tristram", "Cathedral", "Catacombs", "Caves", "Hell", "Main Theme" }; /* NDS-IDIOMA */
const char *const NdsCineNamesEn[4] = { "Intro", "The Monastery", "Diablo Defeated", "The Hooded One" }; /* NDS-IDIOMA */
bool NdsTrackSeen(int i) /* NDS-MUSICAS: solo las que el jugador ya ha escuchado */
{
	char n[8] = "mus0";
	n[3] = static_cast<char>('0' + static_cast<int>(NdsTracks[i]));
	return nds_cine_seen(n) != 0;
}
constexpr int NdsBestiaryValue = 105; /* NDS-BESTIARIO + NDS-BESTIARIO3 */
extern "C" void nds_top_list(const unsigned char *pix, int pitch); /* NDS-BESTIARIO6 */
void NdsShowBestiary()
{
	/* monstruos matados: lineas "bN" de saves/cinematicas.txt (el directorio actual es devilutionx) */
	std::vector<bool> killed(MonstersData.size(), false);
	if (FILE *f = std::fopen("saves/cinematicas.txt", "r")) {
		char line[48];
		while (std::fgets(line, sizeof line, f) != nullptr) {
			int id = -1;
			if (line[0] == 'b' && std::sscanf(line + 1, "%d", &id) == 1 && id >= 0 && static_cast<size_t>(id) < killed.size())
				killed[static_cast<size_t>(id)] = true;
		}
		std::fclose(f);
	}
	/* NDS-BESTIARIODEX: todos los monstruos que existen en este juego */
	std::vector<bool> usedByUnique(MonstersData.size(), false);
	for (const UniqueMonsterData &u : UniqueMonstersData)
		if (u.mtype >= 0 && static_cast<size_t>(u.mtype) < usedByUnique.size())
			usedByUnique[static_cast<size_t>(u.mtype)] = true;
	std::vector<int> ids;
	int killCount = 0;
	for (size_t i = 0; i < MonstersData.size(); i++) {
		const MonsterData &md = MonstersData[i];
		if (md.name.empty() || static_cast<int>(i) == MT_GOLEM)
			continue;
		if (!gbIsHellfire && static_cast<int>(i) > MT_DIABLO)
			continue;
		if (static_cast<int>(i) != MT_DIABLO && !usedByUnique[i]) {
			if (md.availability == MonsterAvailability::Never)
				continue;
			if (gbIsSpawn && md.availability == MonsterAvailability::Retail)
				continue;
		}
		ids.push_back(static_cast<int>(i));
		if (killed[i])
			killCount++;
	}
	auto known = [&](int idx) { return killed[static_cast<size_t>(ids[static_cast<size_t>(idx)])]; };

	/* paleta de la catedral (l1_1.pal) o, si no esta, la del pueblo (NDS-BESTIARIO2) */
	ArtBackground = std::nullopt;
	ArtBackgroundWidescreen = std::nullopt;
	{
		std::array<uint8_t, 768> pal {};
		if (!LoadFileInMemWithStatus("levels\\l1data\\l1_1.pal", pal.data(), pal.size()).has_value())
			(void)LoadFileInMemWithStatus("levels\\towndata\\town.pal", pal.data(), pal.size());
		for (size_t i = 0; i < 256; i++) {
			logical_palette[i].r = pal[i * 3];
			logical_palette[i].g = pal[i * 3 + 1];
			logical_palette[i].b = pal[i * 3 + 2];
		}
	}
	UpdateSystemPalette(logical_palette);

	const Point ui = GetUIRectangle().position;
	constexpr int ListY0 = 260, ListTop = 30, RowH = 14, Rows = 10; /* NDS-BESTIARIO6: lista de la pantalla de arriba */
	constexpr int SprCx = 320, SprBottom = 372, SprMaxW = 560, SprMaxH = 330; /* sprite en medio de la tactil */
	int sel = 0, top = 0, loadedFor = -1;
	OptionalOwnedClxSpriteListOrSheet spr, sprDeath;
	std::array<uint8_t, 256> trn {};
	bool hasTrn = false;
	std::unique_ptr<TSnd> deathSnd;
	bool dying = false;
	uint32_t dieStart = 0;
	bool dead = false, attacking = false; /* NDS-BESTIARIO4 */
	uint32_t idleSince = 0, attackStart = 0;
	OptionalOwnedClxSpriteListOrSheet sprAttack;
	std::unique_ptr<TSnd> attackSnd;
	bool done = false;
	SDL_Event event;
	auto loadSel = [&]() {
		const MonsterData &md = MonstersData[static_cast<size_t>(ids[static_cast<size_t>(sel)])];
		spr = std::nullopt;
		sprDeath = std::nullopt;
		deathSnd = nullptr;
		sprAttack = std::nullopt; /* NDS-BESTIARIO4: otro monstruo: de pie y sin prisa */
		attackSnd = nullptr;
		dead = false;
		attacking = false;
		idleSince = SDL_GetTicks();
		if (!known(sel)) { /* NDS-BESTIARIODEX: sin matar, sin sprite */
			hasTrn = false;
			loadedFor = sel;
			dying = false;
			return;
		}
		spr = LoadOptionalClxListOrSheet(StrCat("monsters\\", md.spritePath(), "n.clx").c_str());
		hasTrn = false;
		if (!md.trnFile.empty()) {
			if (LoadFileInMemWithStatus(StrCat("monsters\\", md.trnFile, ".trn").c_str(), trn.data(), trn.size()).has_value()) {
				for (uint8_t &c : trn)
					if (c == 255)
						c = 0;
				hasTrn = true;
			}
		}
		loadedFor = sel;
		dying = false;
	};
	auto startDeath = [&]() {
		if (ids.empty() || !known(sel) || dead || dying) /* NDS-BESTIARIODEX + NDS-BESTIARIO4: muerto se queda muerto */
			return;
		attacking = false;
		const MonsterData &md = MonstersData[static_cast<size_t>(ids[static_cast<size_t>(sel)])];
		if (!sprDeath)
			sprDeath = LoadOptionalClxListOrSheet(StrCat("monsters\\", md.spritePath(), "d.clx").c_str());
		if (!deathSnd)
			deathSnd = sound_file_load(StrCat("monsters\\", md.soundPath(), "d1.wav").c_str());
		if (deathSnd)
			snd_play_snd(deathSnd.get(), 0, 0, *GetOptions().Audio.soundVolume);
		dying = sprDeath.has_value();
		dieStart = SDL_GetTicks();
	};
	while (!done) {
		if (!ids.empty() && loadedFor != sel)
			loadSel();

		SDL_FillSurfaceRect(DiabloUiSurface(), nullptr, 0);
		const Surface out(DiabloUiSurface());
		{ /* NDS-BESTIARIO6: la lista se dibuja en una zona oculta (x 0-255, filas 260-451) y va a la pantalla de arriba */
			for (int y = 0; y < 192; y++)
				std::memset(out.at(0, ListY0 + y), 0, 256);
			const Surface topOut = out.subregion(0, ListY0, 256, 192);
			DrawString(topOut, NDS_T("BestiarioDex", "BestiaryDex"), { { 0, 2 }, { 256, 26 } }, { .flags = UiFlags::FontSize24 | UiFlags::ColorGold | UiFlags::AlignCenter });
			for (int r = 0; r < Rows && top + r < static_cast<int>(ids.size()); r++) {
				const int idx = top + r;
				const MonsterData &mr = MonstersData[static_cast<size_t>(ids[static_cast<size_t>(idx)])];
				char num[8]; /* numero de 3 cifras y ??? si no lo has matado */
				std::snprintf(num, sizeof num, "%03d ", idx + 1);
				const std::string txt = StrCat(idx == sel ? "> " : "", num, known(idx) ? mr.name : std::string("???"));
				DrawString(topOut, txt, { { 8, ListTop + r * RowH }, { 244, RowH } },
				    { .flags = UiFlags::FontSize12 | (idx == sel ? UiFlags::ColorWhitegold : UiFlags::ColorGold) });
			}
			if (!ids.empty())
				DrawString(topOut, StrCat(NDS_T("Matados ", "Killed "), killCount, " / ", static_cast<int>(ids.size())), { { 0, ListTop + Rows * RowH + 4 }, { 256, 14 } },
				    { .flags = UiFlags::FontSize12 | UiFlags::ColorWhitegold | UiFlags::AlignCenter });
			nds_top_list(out.at(0, ListY0), out.pitch());
		}
		if (ids.empty()) {
			DrawString(out, NDS_T("Aun no has matado ningun monstruo", "You have not killed any monster yet"), { { ui.x, ui.y + 200 }, { 640, 40 } }, { .flags = UiFlags::FontSize24 | UiFlags::ColorWhitegold | UiFlags::AlignCenter });
		} else {
			const MonsterData &md = MonstersData[static_cast<size_t>(ids[static_cast<size_t>(sel)])];
			/* fotograma: muerte (una vez y se queda 1 s en el ultimo) o quieto en bucle */
			/* NDS-BESTIARIO4: tras 10 s mirandolo, ataca (y otra vez cada 10 s) */
			if (!dying && !dead && !attacking && known(sel) && SDL_GetTicks() - idleSince >= 10000) {
				if (!sprAttack)
					sprAttack = LoadOptionalClxListOrSheet(StrCat("monsters\\", md.spritePath(), "a.clx").c_str());
				if (!attackSnd)
					attackSnd = sound_file_load(StrCat("monsters\\", md.soundPath(), "a1.wav").c_str());
				if (attackSnd)
					snd_play_snd(attackSnd.get(), 0, 0, *GetOptions().Audio.soundVolume);
				attacking = sprAttack.has_value();
				attackStart = SDL_GetTicks();
				idleSince = attackStart;
			}
			const bool showDeath = dying || dead;
			const OptionalOwnedClxSpriteListOrSheet &anim = showDeath ? sprDeath : (attacking ? sprAttack : spr);
			if (anim) {
				const int animIdx = showDeath ? 4 : (attacking ? 2 : 0);
				const ClxSpriteList list = (*anim).isSheet() ? (*anim).sheet()[0] : (*anim).list();
				const int nFrames = std::max<int>(1, std::min<int>(md.frames[animIdx] > 0 ? md.frames[animIdx] : 1, static_cast<int>(list.numSprites())));
				const uint32_t step = static_cast<uint32_t>(std::max<int>(1, md.rate[animIdx]) * 50);
				int frame;
				if (showDeath) { /* NDS-BESTIARIO4: la muerte termina en el suelo y ahi se queda */
					const uint32_t t = SDL_GetTicks() - dieStart;
					frame = static_cast<int>(t / step);
					if (frame >= nFrames) {
						frame = nFrames - 1;
						dying = false;
						dead = true;
					}
				} else if (attacking) {
					const uint32_t t = SDL_GetTicks() - attackStart;
					frame = static_cast<int>(t / step);
					if (frame >= nFrames) {
						frame = nFrames - 1;
						attacking = false;
						idleSince = SDL_GetTicks();
					}
				} else {
					frame = static_cast<int>((SDL_GetTicks() / step) % static_cast<uint32_t>(nFrames));
				}
				const ClxSprite s = list[static_cast<size_t>(frame)];
				/* dibujar a 1:1 en la zona oculta (x 0-255, filas 0-255) y copiar ampliado 1,5x */
				const int sw = std::min<int>(s.width(), 256), sh = std::min<int>(s.height(), 256);
				for (int y = 0; y < 256; y++)
					std::memset(out.at(0, y), 0, 256);
				const Surface scratch = out.subregion(0, 0, 256, 256);
				if (hasTrn)
					ClxDrawTRN(scratch, { 0, 255 }, s, trn.data());
				else
					ClxDraw(scratch, { 0, 255 }, s);
				const int k10 = std::max(10, std::min(25, std::min(SprMaxW * 10 / std::max(sw, 1), SprMaxH * 10 / std::max(sh, 1)))); /* NDS-BESTIARIO6: hasta 2,5x */
				const int dw = sw * k10 / 10, dh = sh * k10 / 10;
				const int dx0 = ui.x + SprCx - dw / 2, dy0 = ui.y + SprBottom - dh;
				for (int y = 0; y < dh; y++) {
					const uint8_t *src = out.at(0, 256 - sh + y * 10 / k10);
					uint8_t *dst = out.at(dx0, dy0 + y);
					for (int x = 0; x < dw; x++) {
						const uint8_t v = src[x * 10 / k10];
						if (v != 0)
							dst[x] = v;
					}
				}
				for (int y = 0; y < 256; y++) /* limpiar la zona oculta */
					std::memset(out.at(0, y), 0, 256);
			}
			if (!known(sel)) /* NDS-BESTIARIO6: hueco del sprite, en medio */
				DrawString(out, "???", { { ui.x, ui.y + 170 }, { 640, 60 } }, { .flags = UiFlags::FontSize42 | UiFlags::ColorGold | UiFlags::AlignCenter });
			DrawString(out, "<", { { ui.x + 12, ui.y + 190 }, { 60, 50 } }, { .flags = UiFlags::FontSize42 | UiFlags::ColorGold }); /* tocar los lados: anterior / siguiente */
			DrawString(out, ">", { { ui.x + 568, ui.y + 190 }, { 60, 50 } }, { .flags = UiFlags::FontSize42 | UiFlags::ColorGold | UiFlags::AlignRight });
			DrawString(out, known(sel) ? md.name : std::string("???"), { { ui.x, ui.y + 378 }, { 640, 44 } }, { .flags = UiFlags::FontSize30 | UiFlags::ColorWhitegold | UiFlags::AlignCenter });
			{ /* NDS-BESTIARIO6: puntos de vida (en 1 jugador el juego los divide entre 2) */
				std::string hp = NDS_T("Vida: ???", "Life: ???");
				if (known(sel)) {
					const int mn = md.hitPointsMinimum, mx = md.hitPointsMaximum;
					hp = StrCat(NDS_T("Vida: ", "Life: "), std::max(mn / 2, 1), "-", std::max(mx / 2, 1), NDS_T("     Multijugador: ", "     Multiplayer: "), mn, "-", mx);
				}
				DrawString(out, hp, { { ui.x, ui.y + 426 }, { 640, 30 } }, { .flags = UiFlags::FontSize24 | UiFlags::ColorGold | UiFlags::AlignCenter });
			}
		}
		UiFadeIn();

		while (PollEvent(&event)) {
			for (const MenuAction a : GetMenuActions(event)) {
				if (a == MenuAction_BACK)
					done = true;
				else if (a == MenuAction_SELECT)
					startDeath();
				else if (a == MenuAction_UP && sel > 0)
					sel--;
				else if (a == MenuAction_DOWN && sel + 1 < static_cast<int>(ids.size()))
					sel++;
				else if (a == MenuAction_LEFT && sel > 0) /* NDS-BESTIARIO6 */
					sel--;
				else if (a == MenuAction_RIGHT && sel + 1 < static_cast<int>(ids.size()))
					sel++;
				else if (a == MenuAction_PAGE_UP)
					sel = std::max(0, sel - Rows);
				else if (a == MenuAction_PAGE_DOWN && !ids.empty())
					sel = std::min(static_cast<int>(ids.size()) - 1, sel + Rows);
			}
			if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) { /* NDS-BESTIARIO6: la tactil es el monstruo */
				const int mx = event.button.x - ui.x;
				if (ids.empty()) {
					done = true; /* sin monstruos: volver */
				} else if (mx < 140) {
					if (sel > 0) /* izquierda: el anterior */
						sel--;
				} else if (mx >= 500) {
					if (sel + 1 < static_cast<int>(ids.size())) /* derecha: el siguiente */
						sel++;
				} else {
					startDeath(); /* en medio: se muere */
				}
			}
			UiHandleEvents(&event);
		}
		if (sel < top)
			top = sel;
		if (sel >= top + Rows)
			top = sel - Rows + 1;
	}
	spr = std::nullopt;
	sprDeath = std::nullopt;
	deathSnd = nullptr;
	nds_top_list(nullptr, 0); /* NDS-BESTIARIO6: arriba vuelve la imagen de los menus */
}
const char *const NdsCineFiles[4][3] = { { "diablo1", nullptr, nullptr }, { "fprst3", nullptr, nullptr }, { "diabvic1", "diabvic2", "diabvic3" }, { "diabend", nullptr, nullptr } };
const char *NdsCineFirstSeen(int i)
{
	if (i == 0)
		return NdsCineFiles[0][0]; /* NDS-MENU2: la intro siempre */
	for (int j = 0; j < 3; j++)
		if (NdsCineFiles[i][j] != nullptr && nds_cine_seen(NdsCineFiles[i][j]) != 0)
			return NdsCineFiles[i][j];
	return nullptr;
}
void NdsPlayCine(int i)
{
	const char *f = NdsCineFirstSeen(i);
	if (f == nullptr)
		return;
	const std::string path = StrCat("gendata\\", f, ".smk");
	play_movie(path.c_str(), true);
}
#endif

void UiMainMenuSelect(size_t value)
{
	MainMenuResult = (_mainmenu_selections)vecMenuItems[value]->m_value;
}

#ifndef NOEXIT
void MainmenuEsc()
{
#ifdef __NDS__ /* NDS-MENU: en los submenus, B vuelve atras */
	if (NdsMenuMode != 0) {
		MainMenuResult = static_cast<_mainmenu_selections>(NdsBackValue);
		return;
	}
#endif
	const std::size_t last = vecMenuItems.size() - 1;
	if (SelectedItem == last) {
		UiMainMenuSelect(last);
	} else {
		SelectedItem = last;
	}
}
#endif

void MainmenuLoad(const char *name)
{
#ifdef __NDS__ /* NDS-MENU */
	if (NdsMenuMode == 1) {
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Cinematicas", "Cinematics")), NdsCineMenuValue));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Soundtrack"), NdsSoundValue)); /* NDS-MENU2 */
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("BestiarioDex", "BestiaryDex")), NdsBestiaryValue)); /* NDS-BESTIARIO + NDS-BESTIARIODEX */
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Credits"), MAINMENU_SHOW_CREDITS));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Creditos DSI Ver.", "DSi Ver. Credits")), NdsCreditsValue));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Volver", "Back")), NdsBackValue));
	} else if (NdsMenuMode == 3) { /* NDS-MENU2: Soundtrack */
		for (int i = 0; i < 6; i++)
			vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NdsTrackSeen(i) ? (NdsEs() ? NdsTrackNames[i] : NdsTrackNamesEn[i]) : "???"), NdsTrackSeen(i) ? NdsTrackBase + i : NdsLockedValue)); /* NDS-MUSICAS */
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Volver", "Back")), NdsBackValue));
	} else if (NdsMenuMode == 4) { /* NDS-IDIOMA */
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NdsEs() ? "English" : "> English"), NdsLangEn));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NdsEs() ? "> Espa\xC3\xB1ol" : "Espa\xC3\xB1ol"), NdsLangEs));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Volver", "Back")), NdsBackValue));
	} else if (NdsMenuMode == 5) { /* NDS-OPCIONES: Opciones */
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Pantallas", "Screens")), NdsScreensValue));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Idioma", "Language")), NdsLangValue));
		if (HaveHellfire()) /* NDS-JUEGO: solo con Hellfire en la SD */
			vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Juego", "Game")), NdsGameValue));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Volver", "Back")), NdsBackValue));
	} else if (NdsMenuMode == 7) { /* NDS-JUEGO: Diablo / Hellfire */
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(gbIsHellfire ? "Diablo" : "> Diablo"), NdsGameDiablo));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(gbIsHellfire ? "> Hellfire" : "Hellfire"), NdsGameHellfire));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Volver", "Back")), NdsBackValue));
	} else if (NdsMenuMode == 6) { /* NDS-OPCIONES: Pantallas */
		const bool arriba = nds_get_swap() != 0;
		const char *txtAbajo = NdsEs() ? (arriba ? "Juego abajo" : "> Juego abajo") : (arriba ? "Game below" : "> Game below"); /* NDS-PANTALLAS2 */
		const char *txtArriba = NdsEs() ? (arriba ? "> Juego arriba" : "Juego arriba") : (arriba ? "> Game above" : "Game above");
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(txtAbajo), NdsScrBottom));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(txtArriba), NdsScrTop));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Volver", "Back")), NdsBackValue));
	} else if (NdsMenuMode == 2) {
		for (int i = 0; i < 4; i++) {
			const bool seen = NdsCineFirstSeen(i) != nullptr;
			vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(seen ? (NdsEs() ? NdsCineNames[i] : NdsCineNamesEn[i]) : "???"), seen ? NdsCineBase + i : NdsLockedValue));
		}
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Volver", "Back")), NdsBackValue));
	} else {
		vecMenuItems.push_back(std::make_unique<UiListItem>(_("Single Player"), MAINMENU_SINGLE_PLAYER));
		vecMenuItems.push_back(std::make_unique<UiListItem>(_("Multi Player"), MAINMENU_MULTIPLAYER));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Extras"), NdsExtrasValue));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NDS_T("Opciones", "Options")), NdsOptionsValue)); /* NDS-OPCIONES */
		vecMenuItems.push_back(std::make_unique<UiListItem>(_("Support"), MAINMENU_SHOW_SUPPORT));
		vecMenuItems.push_back(std::make_unique<UiListItem>(gbIsHellfire ? _("Exit Hellfire") : _("Exit Diablo"), MAINMENU_EXIT_DIABLO));
	}
#else
	vecMenuItems.push_back(std::make_unique<UiListItem>(_("Single Player"), MAINMENU_SINGLE_PLAYER));
	vecMenuItems.push_back(std::make_unique<UiListItem>(_("Multi Player"), MAINMENU_MULTIPLAYER));
#ifndef __NDS__ /* NDS-CREDITOS: sin Settings en la DSi */
	vecMenuItems.push_back(std::make_unique<UiListItem>(_("Settings"), MAINMENU_SETTINGS));
#endif
	vecMenuItems.push_back(std::make_unique<UiListItem>(_("Support"), MAINMENU_SHOW_SUPPORT));
	vecMenuItems.push_back(std::make_unique<UiListItem>(_("Show Credits"), MAINMENU_SHOW_CREDITS));
#ifdef __NDS__ /* NDS-CREDITOS */
	vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Creditos DSI Ver."), NdsCreditsValue));
#endif
#ifndef NOEXIT
	vecMenuItems.push_back(std::make_unique<UiListItem>(gbIsHellfire ? _("Exit Hellfire") : _("Exit Diablo"), MAINMENU_EXIT_DIABLO));
#endif
#endif /* NDS-MENU */

	if (!gbIsSpawn || gbIsHellfire) {
		ArtBackgroundWidescreen = LoadOptionalClx("ui_art\\mainmenuw.clx");
		LoadBackgroundArt("ui_art\\mainmenu");
	} else {
		LoadBackgroundArt("ui_art\\swmmenu");
	}

	UiAddBackground(&vecMainMenuDialog);
	UiAddLogo(&vecMainMenuDialog);

	const Point uiPosition = GetUIRectangle().position;

	if (gbIsSpawn && gbIsHellfire) {
		const SDL_Rect rect1 = { (Sint16)(uiPosition.x), (Sint16)(uiPosition.y + 145), 640, 30 };
		vecMainMenuDialog.push_back(std::make_unique<UiArtText>(_("Shareware").data(), rect1, UiFlags::FontSize30 | UiFlags::ColorUiSilver | UiFlags::AlignCenter, 8));
	}

#ifdef __NDS__ /* NDS-MENU2: listas largas con letra algo mas pequena */
	{
		const bool ndsLarga = NdsMenuMode != 0 && vecMenuItems.size() > 5; /* NDS-OPCIONES */
		const int ndsAlto = ndsLarga ? 35 : (vecMenuItems.size() > 5 ? 39 : 43); /* NDS-MENUHUECO */
		vecMainMenuDialog.push_back(std::make_unique<UiList>(vecMenuItems, vecMenuItems.size(), uiPosition.x + 64, (uiPosition.y + 192), 510, ndsAlto,
		    (ndsLarga ? UiFlags::FontSize30 : UiFlags::FontSize42) | UiFlags::ColorUiGold | UiFlags::AlignCenter, 5));
	}
#else
	vecMainMenuDialog.push_back(std::make_unique<UiList>(vecMenuItems, vecMenuItems.size(), uiPosition.x + 64, (uiPosition.y + 192), 510, 43, UiFlags::FontSize42 | UiFlags::ColorUiGold | UiFlags::AlignCenter, 5));
#endif

#ifdef __NDS__ /* NDS-FRONT + NDS-VERSION: letra 24 centrada, un poco mas arriba */
	const SDL_Rect rect2 = { (Sint16)(uiPosition.x), (Sint16)(uiPosition.y + 480 - 28), 640, 26 }; /* NDS-MENUHUECO: texto gris mas abajo */
	vecMainMenuDialog.push_back(std::make_unique<UiArtText>(name, rect2, UiFlags::FontSize24 | UiFlags::ColorUiSilver | UiFlags::AlignCenter));
#else
	const SDL_Rect rect2 = { 17, (Sint16)(gnScreenHeight - 36), 605, 21 };
	vecMainMenuDialog.push_back(std::make_unique<UiArtText>(name, rect2, UiFlags::FontSize12 | UiFlags::ColorUiSilverDark));
#endif

#ifndef NOEXIT
	UiInitList(nullptr, UiMainMenuSelect, MainmenuEsc, vecMainMenuDialog, true);
#else
	UiInitList(nullptr, UiMainMenuSelect, nullptr, vecMainMenuDialog, true);
#endif
}

void MainmenuFree()
{
	ArtBackgroundWidescreen = std::nullopt;
	ArtBackground = std::nullopt;

	vecMainMenuDialog.clear();

	vecMenuItems.clear();
}

} // namespace

void mainmenu_restart_repintro()
{
	dwAttractTicks = SDL_GetTicks() + mainmenu_attract_time_out * 1000;
}

bool UiMainMenuDialog(const char *name, _mainmenu_selections *pdwResult, int attractTimeOut)
{
	MainMenuResult = MAINMENU_NONE;
	while (MainMenuResult == MAINMENU_NONE) {
		mainmenu_attract_time_out = attractTimeOut;
		MainmenuLoad(name);

		mainmenu_restart_repintro(); // for automatic starts

		while (MainMenuResult == MAINMENU_NONE) {
#ifdef __NDS__ /* NDS-V12: version arriba solo en el menu principal (no en Extras) */
			nds_ui_mainmenu(NdsMenuMode == 0 ? 1 : 0);
			nds_set_lang_es(NdsEs() ? 1 : 0); /* NDS-IDIOMA */
#endif
			UiClearScreen();
			UiPollAndRender();
#ifdef __NDS__ /* NDS-ATRACCION: solo en el menu principal, no en los submenus */
			if (NdsMenuMode == 0 && SDL_GetTicks() >= dwAttractTicks && (HaveIntro() || gbIsHellfire)) {
#else
			if (SDL_GetTicks() >= dwAttractTicks && (HaveIntro() || gbIsHellfire)) {
#endif
				MainMenuResult = MAINMENU_ATTRACT_MODE;
			}
		}

		MainmenuFree();
#ifdef __NDS__ /* NDS-V12 */
		nds_ui_mainmenu(0);
#endif
#ifdef __NDS__ /* NDS-CREDITOS / NDS-MENU: submenus sin salir del dialogo */
		{
			const int r = static_cast<int>(MainMenuResult);
			if (r == NdsCreditsValue) {
				NdsShowDsiCredits();
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsExtrasValue) {
				NdsMenuMode = 1;
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsCineMenuValue) {
				NdsMenuMode = 2;
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsBestiaryValue) { /* NDS-BESTIARIO */
				NdsShowBestiary();
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsSoundValue) { /* NDS-MENU2 */
				NdsMenuMode = 3;
				MainMenuResult = MAINMENU_NONE;
			} else if (r >= NdsTrackBase && r < NdsTrackBase + 6) {
				music_start(NdsTracks[r - NdsTrackBase]);
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsOptionsValue) { /* NDS-OPCIONES */
				NdsMenuMode = 5;
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsScreensValue) {
				NdsMenuMode = 6;
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsScrBottom || r == NdsScrTop) {
				nds_set_swap(r == NdsScrTop ? 1 : 0);
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsGameValue) { /* NDS-JUEGO */
				NdsMenuMode = 7;
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsGameDiablo || r == NdsGameHellfire) { /* NDS-JUEGO */
				const bool hf = r == NdsGameHellfire;
				if (hf == gbIsHellfire) {
					MainMenuResult = MAINMENU_NONE; /* ya es el juego actual */
				} else {
					GetOptions().GameMode.gameMode.SetValue(StartUpGameMode::Diablo); /* sin selector al arrancar: decide [Mods] hf */
					GetOptions().Mods.SetHellfireEnabled(hf);
					SaveOptions();
					UiSelOkDialog(hf ? "Hellfire" : "Diablo", NDS_T("Vuelve a abrir Hellspawn para jugar con el juego elegido.", "Open Hellspawn again to play the chosen game."), false);
					NdsMenuMode = 0;
					MainMenuResult = MAINMENU_EXIT_DIABLO; /* sale igual que "Salir" */
				}
			} else if (r == NdsLangValue) { /* NDS-IDIOMA */
				NdsMenuMode = 4;
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsLangEn || r == NdsLangEs) {
				NdsSetLanguage(r == NdsLangEs ? "es" : "en");
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsBackValue) {
				if (NdsMenuMode == 3)
					music_start(TMUSIC_INTRO); /* al salir del Soundtrack vuelve la musica del menu */
				NdsMenuMode = (NdsMenuMode == 4 || NdsMenuMode == 6 || NdsMenuMode == 7) ? 5 : (NdsMenuMode == 2 || NdsMenuMode == 3) ? 1 : 0; /* NDS-OPCIONES: Idioma y Pantallas vuelven a Opciones */
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsLockedValue) {
				MainMenuResult = MAINMENU_NONE;
			} else if (r >= NdsCineBase && r < NdsCineBase + 4) {
				NdsPlayCine(r - NdsCineBase);
				MainMenuResult = MAINMENU_NONE;
			} else if (r != MAINMENU_SHOW_CREDITS) {
				NdsMenuMode = 0; /* se sale del menu: al volver, el principal (tras Credits se vuelve a Extras) */
			}
		}
#endif
	}

	*pdwResult = MainMenuResult;
	return true;
}

} // namespace devilution
