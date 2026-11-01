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
#endif

namespace devilution {
namespace {
int mainmenu_attract_time_out; // seconds
uint32_t dwAttractTicks;

std::vector<std::unique_ptr<UiItemBase>> vecMainMenuDialog;
std::vector<std::unique_ptr<UiListItem>> vecMenuItems;

_mainmenu_selections MainMenuResult;

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
		{ "Hellspawn - Diablo DSI", 5, UiFlags::FontSize42 | UiFlags::ColorUiGold },
		{ "Agradecimiento especial a:", 55, UiFlags::FontSize30 | UiFlags::ColorUiSilver },
		{ "Night Stroke", 95, UiFlags::FontSize42 | UiFlags::ColorUiGold },
		{ "por la imagen del menu y el icono", 137, UiFlags::FontSize30 | UiFlags::ColorUiSilver },
		{ "Yagatsu18", 179, UiFlags::FontSize42 | UiFlags::ColorUiGold },
		{ "por ser betatester", 221, UiFlags::FontSize30 | UiFlags::ColorUiSilver },
		{ "DevilutionX", 263, UiFlags::FontSize42 | UiFlags::ColorUiGold },
		{ "por el codigo del port", 305, UiFlags::FontSize30 | UiFlags::ColorUiSilver },
		{ "Blizzard North", 347, UiFlags::FontSize42 | UiFlags::ColorUiGold },
		{ "por crear este juegazo", 389, UiFlags::FontSize30 | UiFlags::ColorUiSilver },
		{ "Pulsa un boton para volver", 450, UiFlags::FontSize12 | UiFlags::ColorUiSilverDark },
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
bool NdsTrackSeen(int i) /* NDS-MUSICAS: solo las que el jugador ya ha escuchado */
{
	char n[8] = "mus0";
	n[3] = static_cast<char>('0' + static_cast<int>(NdsTracks[i]));
	return nds_cine_seen(n) != 0;
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
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Cinematicas"), NdsCineMenuValue));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Soundtrack"), NdsSoundValue)); /* NDS-MENU2 */
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Credits"), MAINMENU_SHOW_CREDITS));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Creditos DSI Ver."), NdsCreditsValue));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Volver"), NdsBackValue));
	} else if (NdsMenuMode == 3) { /* NDS-MENU2: Soundtrack */
		for (int i = 0; i < 6; i++)
			vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(NdsTrackSeen(i) ? NdsTrackNames[i] : "???"), NdsTrackSeen(i) ? NdsTrackBase + i : NdsLockedValue)); /* NDS-MUSICAS */
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Volver"), NdsBackValue));
	} else if (NdsMenuMode == 2) {
		for (int i = 0; i < 4; i++) {
			const bool seen = NdsCineFirstSeen(i) != nullptr;
			vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view(seen ? NdsCineNames[i] : "???"), seen ? NdsCineBase + i : NdsLockedValue));
		}
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Volver"), NdsBackValue));
	} else {
		vecMenuItems.push_back(std::make_unique<UiListItem>(_("Single Player"), MAINMENU_SINGLE_PLAYER));
		vecMenuItems.push_back(std::make_unique<UiListItem>(_("Multi Player"), MAINMENU_MULTIPLAYER));
		vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Extras"), NdsExtrasValue));
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
		const bool ndsLarga = vecMenuItems.size() > 5;
		vecMainMenuDialog.push_back(std::make_unique<UiList>(vecMenuItems, vecMenuItems.size(), uiPosition.x + 64, (uiPosition.y + 192), 510, ndsLarga ? 35 : 43,
		    (ndsLarga ? UiFlags::FontSize30 : UiFlags::FontSize42) | UiFlags::ColorUiGold | UiFlags::AlignCenter, 5));
	}
#else
	vecMainMenuDialog.push_back(std::make_unique<UiList>(vecMenuItems, vecMenuItems.size(), uiPosition.x + 64, (uiPosition.y + 192), 510, 43, UiFlags::FontSize42 | UiFlags::ColorUiGold | UiFlags::AlignCenter, 5));
#endif

#ifdef __NDS__ /* NDS-FRONT */
	const SDL_Rect rect2 = { (Sint16)(uiPosition.x + 17), (Sint16)(uiPosition.y + 480 - 36), 605, 21 };
#else
	const SDL_Rect rect2 = { 17, (Sint16)(gnScreenHeight - 36), 605, 21 };
#endif
	vecMainMenuDialog.push_back(std::make_unique<UiArtText>(name, rect2, UiFlags::FontSize12 | UiFlags::ColorUiSilverDark));

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
			UiClearScreen();
			UiPollAndRender();
			if (SDL_GetTicks() >= dwAttractTicks && (HaveIntro() || gbIsHellfire)) {
				MainMenuResult = MAINMENU_ATTRACT_MODE;
			}
		}

		MainmenuFree();
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
			} else if (r == NdsSoundValue) { /* NDS-MENU2 */
				NdsMenuMode = 3;
				MainMenuResult = MAINMENU_NONE;
			} else if (r >= NdsTrackBase && r < NdsTrackBase + 6) {
				music_start(NdsTracks[r - NdsTrackBase]);
				MainMenuResult = MAINMENU_NONE;
			} else if (r == NdsBackValue) {
				if (NdsMenuMode == 3)
					music_start(TMUSIC_INTRO); /* al salir del Soundtrack vuelve la musica del menu */
				NdsMenuMode = NdsMenuMode >= 2 ? 1 : 0;
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
