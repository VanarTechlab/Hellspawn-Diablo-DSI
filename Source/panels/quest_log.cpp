#include "panels/quest_log.hpp"

#include <algorithm>
#include <string_view>

#include "DiabloUI/ui_flags.hpp"
#include "control/control.hpp"
#include "effects.h"
#include "engine/render/clx_render.hpp"
#include "engine/render/text_render.hpp"
#include "minitext.h"
#include "panels/ui_panels.hpp"
#include "quests.h"
#include "utils/language.h"

namespace devilution {

bool QuestLogIsOpen;
OptionalOwnedClxSpriteList pQLogCel;

namespace {

/** Indices of quests to display in quest log window. `FirstFinishedQuest` are active quests the rest are completed */
quest_id EncounteredQuests[MAXQUESTS];
/** Overall number of EncounteredQuests entries */
int EncounteredQuestCount;
/** First (nonselectable) finished quest in list */
int FirstFinishedQuest;
/** Currently selected quest list item */
int SelectedQuest;

constexpr Rectangle InnerPanel { { 32, 26 }, { 280, 300 } };
#ifdef __NDS__ /* NDS-MISIONES: letra grande si caben */
int LineHeight = 12;
bool NdsQLBig = false;
#define MaxSpacing (LineHeight * 2)
#else
constexpr int LineHeight = 12;
constexpr int MaxSpacing = LineHeight * 2;
#endif
int ListYOffset;
int LineSpacing;
/** The number of pixels to move finished quest, to separate them from the active ones */
int FinishedQuestOffset;

int QuestLogMouseToEntry()
{
	Rectangle innerArea = InnerPanel;
	innerArea.position += Displacement(GetLeftPanel().position.x, GetLeftPanel().position.y);
	if (!innerArea.contains(MousePosition) || (EncounteredQuestCount == 0))
		return -1;
	const int y = MousePosition.y - innerArea.position.y;
	for (int i = 0; i < FirstFinishedQuest; i++) {
		if ((y >= ListYOffset + i * LineSpacing)
		    && (y < ListYOffset + i * LineSpacing + LineHeight)) {
			return i;
		}
	}
	return -1;
}

void PrintQLString(const Surface &out, int x, int y, std::string_view str, bool marked, bool disabled = false)
{
#ifdef __NDS__ /* NDS-MISIONES */
	const int width = NdsQLBig ? GetLineWidth(str, GameFont24) : GetLineWidth(str);
	const int markY = NdsQLBig ? 20 : 13;
	const UiFlags sizeFlag = NdsQLBig ? UiFlags::FontSize24 : UiFlags::FontSize12;
#else
	const int width = GetLineWidth(str);
	const int markY = 13;
	const UiFlags sizeFlag = UiFlags::FontSize12;
#endif
#ifdef __NDS__ /* NDS-MISIONES2: con letra grande, centrado en todo el ancho del panel */
	const int areaW = NdsQLBig ? 320 : 257;
	if (NdsQLBig)
		x = 0;
#else
	const int areaW = 257;
#endif
	x += std::max((areaW - width) / 2, 0);
	if (marked) {
		ClxDraw(out, GetPanelPosition(UiPanels::Quest, { x - 20, y + markY }), (*pSPentSpn2Cels)[PentSpn2Spin()]);
	}
	DrawString(out, str, { GetPanelPosition(UiPanels::Quest, { x, y }), { areaW, 0 } },
	    { .flags = sizeFlag | (disabled ? UiFlags::ColorWhitegold : UiFlags::ColorWhite) });
	if (marked) {
		ClxDraw(out, GetPanelPosition(UiPanels::Quest, { x + width + 7, y + markY }), (*pSPentSpn2Cels)[PentSpn2Spin()]);
	}
}

} // namespace

void DrawQuestLog(const Surface &out)
{
	const int l = QuestLogMouseToEntry();
	if (l >= 0) {
		SelectedQuest = l;
	}
	const auto x = InnerPanel.position.x;
#ifdef __NDS__ /* NDS-MISIONES2: con letra grande, sin el marco de piedra (fondo negro) */
	if (!NdsQLBig)
#endif
	ClxDraw(out, GetPanelPosition(UiPanels::Quest, { 0, 351 }), (*pQLogCel)[0]);
	int y = InnerPanel.position.y + ListYOffset;
	for (int i = 0; i < EncounteredQuestCount; i++) {
		if (i == FirstFinishedQuest) {
			y += FinishedQuestOffset;
		}
		PrintQLString(out, x, y, _(QuestsData[EncounteredQuests[i]]._qlstr), i == SelectedQuest, i >= FirstFinishedQuest);
		y += LineSpacing;
	}
}

void StartQuestlog()
{
	auto sortQuestIdx = [](int a, int b) {
		return QuestsData[a].questBookOrder < QuestsData[b].questBookOrder;
	};

	EncounteredQuestCount = 0;
	for (auto &quest : Quests) {
		if (quest._qactive == QUEST_ACTIVE && quest._qlog) {
			EncounteredQuests[EncounteredQuestCount] = quest._qidx;
			EncounteredQuestCount++;
		}
	}
	FirstFinishedQuest = EncounteredQuestCount;
	for (auto &quest : Quests) {
		if (quest._qactive == QUEST_DONE || quest._qactive == QUEST_HIVE_DONE) {
			EncounteredQuests[EncounteredQuestCount] = quest._qidx;
			EncounteredQuestCount++;
		}
	}

	std::sort(&EncounteredQuests[0], &EncounteredQuests[FirstFinishedQuest], sortQuestIdx);
	std::sort(&EncounteredQuests[FirstFinishedQuest], &EncounteredQuests[EncounteredQuestCount], sortQuestIdx);

	const bool twoBlocks = FirstFinishedQuest != 0 && FirstFinishedQuest < EncounteredQuestCount;

#ifdef __NDS__ /* NDS-MISIONES: letra 24 si caben todas las misiones */
	NdsQLBig = EncounteredQuestCount * 26 + 13 <= InnerPanel.size.height;
	LineHeight = NdsQLBig ? 26 : 12;
#endif
	ListYOffset = 0;
	FinishedQuestOffset = !twoBlocks ? 0 : LineHeight / 2;

	const int overallMinHeight = (EncounteredQuestCount * LineHeight) + FinishedQuestOffset;
	const int space = InnerPanel.size.height;

	if (EncounteredQuestCount > 0) {
		const int additionalSpace = space - overallMinHeight;
		int addLineSpacing = additionalSpace / EncounteredQuestCount;
		addLineSpacing = std::min(MaxSpacing - LineHeight, addLineSpacing);
		LineSpacing = LineHeight + addLineSpacing;
		if (twoBlocks) {
			int additionalSepSpace = additionalSpace - (addLineSpacing * EncounteredQuestCount);
			additionalSepSpace = std::min(LineHeight, additionalSepSpace);
			FinishedQuestOffset = std::max(4, additionalSepSpace);
		}

		const int overallHeight = (EncounteredQuestCount * LineSpacing) + FinishedQuestOffset;
		ListYOffset += (space - overallHeight) / 2;
	}

	SelectedQuest = FirstFinishedQuest == 0 ? -1 : 0;
	QuestLogIsOpen = true;
}

void QuestlogUp()
{
	if (FirstFinishedQuest == 0) {
		SelectedQuest = -1;
	} else {
		SelectedQuest--;
		if (SelectedQuest < 0) {
			SelectedQuest = FirstFinishedQuest - 1;
		}
		PlaySFX(SfxID::MenuMove);
	}
}

void QuestlogDown()
{
	if (FirstFinishedQuest == 0) {
		SelectedQuest = -1;
	} else {
		SelectedQuest++;
		if (SelectedQuest == FirstFinishedQuest) {
			SelectedQuest = 0;
		}
		PlaySFX(SfxID::MenuMove);
	}
}

void QuestlogEnter()
{
	PlaySFX(SfxID::MenuSelect);
	if (EncounteredQuestCount != 0 && SelectedQuest >= 0 && SelectedQuest < FirstFinishedQuest)
		InitQTextMsg(Quests[EncounteredQuests[SelectedQuest]]._qmsg);
	QuestLogIsOpen = false;
}

void QuestlogESC()
{
	const int l = QuestLogMouseToEntry();
	if (l != -1) {
		QuestlogEnter();
	}
}

} // namespace devilution
