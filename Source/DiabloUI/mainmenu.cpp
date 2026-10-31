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

void UiMainMenuSelect(size_t value)
{
	MainMenuResult = (_mainmenu_selections)vecMenuItems[value]->m_value;
}

#ifndef NOEXIT
void MainmenuEsc()
{
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

	vecMainMenuDialog.push_back(std::make_unique<UiList>(vecMenuItems, vecMenuItems.size(), uiPosition.x + 64, (uiPosition.y + 192), 510, 43, UiFlags::FontSize42 | UiFlags::ColorUiGold | UiFlags::AlignCenter, 5));

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
#ifdef __NDS__ /* NDS-CREDITOS: se muestra la pantalla y se vuelve al menu */
		if (static_cast<int>(MainMenuResult) == NdsCreditsValue) {
			NdsShowDsiCredits();
			MainMenuResult = MAINMENU_NONE;
		}
#endif
	}

	*pdwResult = MainMenuResult;
	return true;
}

} // namespace devilution
