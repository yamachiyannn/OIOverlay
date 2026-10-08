//
// OIOverlay
// Initial in-game overlay test
//

#include <Windows.h>
#include <SokuLib.hpp>

static bool initialized = false;
static bool overlayEnabled = true;
static bool previousF10State = false;

static int (SokuLib::BattleManager::*ogBattleMgrOnProcess)();
static void (SokuLib::BattleManager::*ogBattleMgrOnRender)();

static SokuLib::DrawUtils::Sprite text;
static SokuLib::SWRFont font;


// ------------------------------------------------------------
// Font
// ------------------------------------------------------------

void loadFont()
{
	SokuLib::FontDescription desc;

	// White
	desc.r1 = 255;
	desc.g1 = 255;
	desc.b1 = 255;

	// White
	desc.r2 = 255;
	desc.g2 = 255;
	desc.b2 = 255;

	desc.height = 24;
	desc.weight = FW_BOLD;
	desc.italic = 0;
	desc.shadow = 4;
	desc.bufferSize = 1000000;
	desc.charSpaceX = 0;
	desc.charSpaceY = 0;
	desc.offsetX = 0;
	desc.offsetY = 0;
	desc.useOffset = 0;

	strcpy(desc.faceName, "MonoSpatialModSWR");

	font.create();
	font.setIndirect(desc);
}


// ------------------------------------------------------------
// Create overlay text
// ------------------------------------------------------------

void createOverlay()
{
	SokuLib::Vector2i realSize;

	text.texture.createFromText(
		"OIOverlay : ON",
		font,
		{300, 300},
		&realSize
	);

	// Top-left area
	text.setPosition(SokuLib::Vector2i{20, 20});

	text.setSize(realSize.to<unsigned>());

	text.rect.width = realSize.x;
	text.rect.height = realSize.y;
}


// ------------------------------------------------------------
// BattleManager::onRender hook
// ------------------------------------------------------------

int __fastcall CBattleManager_OnRender(SokuLib::BattleManager *This)
{
	// Original render
	(This->*ogBattleMgrOnRender)();

	// Draw our overlay
	if (initialized && overlayEnabled)
		text.draw();

	return 0;
}


// ------------------------------------------------------------
// BattleManager::onProcess hook
// ------------------------------------------------------------

int __fastcall CBattleManager_OnProcess(SokuLib::BattleManager *This)
{
	if (!initialized) {
		loadFont();
		createOverlay();

		initialized = true;
	}

	// --------------------------------------------------------
	// F10 toggle
	// --------------------------------------------------------

	bool f10State =
		(GetAsyncKeyState(VK_F10) & 0x8000) != 0;

	// Detect key press instead of holding the key
	if (f10State && !previousF10State) {
		overlayEnabled = !overlayEnabled;
	}

	previousF10State = f10State;

	// Call original process
	return (This->*ogBattleMgrOnProcess)();
}


// ------------------------------------------------------------
// Check game version
// ------------------------------------------------------------

extern "C" __declspec(dllexport)
bool CheckVersion(const BYTE hash[16])
{
	return memcmp(
		hash,
		SokuLib::targetHash,
		sizeof(SokuLib::targetHash)
	) == 0;
}


// ------------------------------------------------------------
// Initialize
// ------------------------------------------------------------

extern "C" __declspec(dllexport)
bool Initialize(HMODULE hMyModule, HMODULE hParentModule)
{
	DWORD old;

#ifdef _DEBUG
	FILE *_;

	AllocConsole();

	freopen_s(&_, "CONOUT$", "w", stdout);
	freopen_s(&_, "CONOUT$", "w", stderr);
#endif

	puts("OIOverlay: Initialize");

	// --------------------------------------------------------
	// Hook BattleManager
	// --------------------------------------------------------

	VirtualProtect(
		(PVOID)RDATA_SECTION_OFFSET,
		RDATA_SECTION_SIZE,
		PAGE_EXECUTE_WRITECOPY,
		&old
	);

	ogBattleMgrOnRender =
		SokuLib::TamperDword(
			&SokuLib::VTable_BattleManager.onRender,
			CBattleManager_OnRender
		);

	ogBattleMgrOnProcess =
		SokuLib::TamperDword(
			&SokuLib::VTable_BattleManager.onProcess,
			CBattleManager_OnProcess
		);

	VirtualProtect(
		(PVOID)RDATA_SECTION_OFFSET,
		RDATA_SECTION_SIZE,
		old,
		&old
	);

	FlushInstructionCache(
		GetCurrentProcess(),
		nullptr,
		0
	);

	puts("OIOverlay: Hooks installed");

	return true;
}


// ------------------------------------------------------------
// DLL entry point
// ------------------------------------------------------------

extern "C"
int APIENTRY DllMain(
	HMODULE hModule,
	DWORD fdwReason,
	LPVOID lpReserved
)
{
	return TRUE;
}


// ------------------------------------------------------------
// Mod loading priority
// ------------------------------------------------------------

extern "C" __declspec(dllexport)
int getPriority()
{
	return 0;
}
