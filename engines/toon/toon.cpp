/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/system.h"
#include "common/events.h"
#include "common/debug-channels.h"
#include "common/archive.h"
#include "common/config-manager.h"
#include "common/savefile.h"
#include "common/memstream.h"
#include "common/translation.h"

#include "audio/mididrv.h"
#include "engines/advancedDetector.h"
#include "engines/util.h"
#include "graphics/palette.h"
#include "graphics/surface.h"
#include "graphics/thumbnail.h"
#include "gui/saveload.h"
#include "gui/message.h"
#include "toon/resource.h"
#include "toon/toon.h"
#include "toon/anim.h"
#include "toon/picture.h"
#include "toon/hotspot.h"
#include "toon/flux.h"
#include "toon/drew.h"
#include "toon/path.h"

namespace Toon {

void ToonEngine::init() {
	// Assign default values to the ScummVM configuration manager, in case settings are missing
	ConfMan.registerDefault("music_volume", 192);
	ConfMan.registerDefault("speech_volume", 192);
	ConfMan.registerDefault("sfx_volume", 192);
	ConfMan.registerDefault("music_mute", "false");
	ConfMan.registerDefault("speech_mute", "false");
	ConfMan.registerDefault("sfx_mute", "false");
	ConfMan.registerDefault("mute", "false");
	ConfMan.registerDefault("subtitles", "true");
	ConfMan.registerDefault("talkspeed", 60); // Can go up to 255
	if (!_isEnglishDemo) {
		ConfMan.registerDefault("alternative_font", "false");
	}

	_currentScriptRegion = 0;
	_resources = new Resources(this);
	_animationManager = new AnimationManager(this);
	_moviePlayer = new Movie(this, new ToonstruckSmackerDecoder());
	_hotspots = new Hotspots(this);

	_mainSurface = new Graphics::Surface();
	_mainSurface->create(TOON_BACKBUFFER_WIDTH, TOON_BACKBUFFER_HEIGHT, Graphics::PixelFormat::createFormatCLUT8());

	_finalPalette = new uint8[768]();
	_backupPalette = new uint8[768]();
	_additionalPalette1 = new uint8[69]();
	_additionalPalette2 = new uint8[69]();
	_cutawayPalette = new uint8[768]();
	_universalPalette = new uint8[96]();
	_fluxPalette = new uint8[24]();

	_conversationData = new int16[4096]();

	_shouldQuit = false;
	_scriptStep = 0;

	_cursorOffsetX = 0;
	_cursorOffsetY = 0;
	_currentHotspotItem = 0;

	_currentTextLine = 0;
	_currentTextLineId = -1;
	_currentTextLineX = 0;
	_currentTextLineY = 0;
	_currentTextLineCharacterId = 0;

	_saveBufferStream = new Common::MemoryWriteStreamDynamic(DisposeAfterUse::YES);

	_firstFrame = false;

	const Common::FSNode gameDataDir(ConfMan.getPath("path"));
	SearchMan.addSubDirectoryMatching(gameDataDir, "MISC");
	SearchMan.addSubDirectoryMatching(gameDataDir, "ACT1");
	SearchMan.addSubDirectoryMatching(gameDataDir, "ACT2");

	_pathFinding = new PathFinding();

	resources()->openPackage("LOCAL.PAK");
	resources()->openPackage("ONETIME.PAK");
	resources()->openPackage("DREW.PAK");

	// load subtitles if available (if fails to load it only return false, so there's no need to check)
	resources()->openPackage("SUBTITLES.PAK");

	for (int32 i = 0; i < 32; ++i)
		_characters[i] = nullptr;

	_characters[0] = new CharacterDrew(this);
	_characters[1] = new CharacterFlux(this);
	_drew = _characters[0];
	_flux = _characters[1];

	// preload walk anim for flux and drew
	_drew->loadWalkAnimation("STNDWALK.CAF");
	_drew->setupPalette();
	_drew->loadShadowAnimation("SHADOW.CAF");

	_flux->loadWalkAnimation("FXSTWALK.CAF");
	_flux->loadShadowAnimation("SHADOW.CAF");

	loadAdditionalPalette("UNIVERSE.PAL", 3);
	loadAdditionalPalette("FLUX.PAL", 4);
	setupGeneralPalette();

	_script_func = new ScriptFunc(this);
	_gameState = new State();
	_gameState->_conversationData = _conversationData;

	memset(_sceneAnimations, 0, sizeof(_sceneAnimations));
	memset(_sceneAnimationScripts, 0, sizeof(_sceneAnimationScripts));

	_drew->setVisible(false);
	_flux->setVisible(false);

	_gameState->_currentChapter = 1;
	initChapter();
	loadCursor();
	initFonts();

	_dialogIcons = new Animation(this);
	_dialogIcons->loadAnimation("DIALOGUE.CAF");

	_inventoryIcons = new Animation(this);
	_inventoryIcons->loadAnimation("INVENTRY.CAF");

	_inventoryIconSlots = new Animation(this);
	_inventoryIconSlots->loadAnimation("ICONSLOT.CAF");

	_genericTexts = new TextResource(this);
	_genericTexts->loadTextResource("GENERIC.TRE");

	_audioManager = new AudioManager(this, _mixer);
	_audioManager->loadAudioPack(0, "GENERIC.SVI", "GENERIC.SVL");
	_audioManager->loadAudioPack(2, "GENERIC.SEI", "GENERIC.SEL");


	// Query the selected music device (defaults to MT_AUTO device).
	MidiDriver::DeviceHandle dev = MidiDriver::getDeviceHandle(ConfMan.hasKey("music_driver") ? ConfMan.get("music_driver") : Common::String("auto"));
	_noMusicDriver = (MidiDriver::getMusicType(dev) == MT_NULL || MidiDriver::getMusicType(dev) == MT_INVALID);

	syncSoundSettings();

	_lastMouseButton = 0;
	_mouseButton = 0;
	_lastRenderTime = _system->getMillis();
}

void ToonEngine::waitForScriptStep() {
	// Wait after a specified number of script steps when executing a script
	// to lower CPU usage
	if (++_scriptStep >= 40) {
		_system->delayMillis(1);
		_scriptStep = 0;
	}
}

void ToonEngine::parseInput() {

	Common::EventManager *_event = _system->getEventManager();

	_mouseX = _event->getMousePos().x;
	_mouseY = _event->getMousePos().y;
	_mouseButton = _event->getButtonState();

	bool breakPollEventloop = false;
	Common::Event event;
	while (!breakPollEventloop && _event->pollEvent(event)) {

		const bool hasModifier = (event.kbd.flags & Common::KBD_NON_STICKY) != 0;
		switch (event.type) {
		case Common::EVENT_MOUSEMOVE:
			_mouseX = event.mouse.x;
			_mouseY = event.mouse.y;
			break;

		case Common::EVENT_LBUTTONDOWN:
			// fall through
		case Common::EVENT_LBUTTONUP:
			// fall through
		case Common::EVENT_RBUTTONDOWN:
			// fall through
		case Common::EVENT_RBUTTONUP:
			_mouseButton = _event->getButtonState();
			breakPollEventloop = true;
			break;

		case Common::EVENT_KEYDOWN:
			if ((event.kbd.keycode == Common::KEYCODE_ESCAPE || event.kbd.keycode == Common::KEYCODE_SPACE) && !hasModifier) {
				_audioManager->stopCurrentVoice();
			}
			if (event.kbd.keycode == Common::KEYCODE_F5 && !hasModifier) {
				if (_gameState->_inMenu) {
					playSoundWrong();
				} else if (canSaveGameStateCurrently())
					saveGame(-1, "");
			}
			if (event.kbd.keycode == Common::KEYCODE_F6 && !hasModifier) {
				if (_gameState->_inMenu) {
					playSoundWrong();
				} else if (canLoadGameStateCurrently())
					loadGame(-1);
			}
			if (event.kbd.keycode == Common::KEYCODE_t && !hasModifier) {
				ConfMan.setBool("subtitles", !ConfMan.getBool("subtitles"));
				syncSoundSettings();
			}
			if (event.kbd.keycode == Common::KEYCODE_m && !hasModifier) {
				ConfMan.setBool("music_mute", !ConfMan.getBool("music_mute"));
				syncSoundSettings();
			}
			if (event.kbd.keycode == Common::KEYCODE_d && !hasModifier) {
				ConfMan.setBool("speech_mute", !ConfMan.getBool("speech_mute"));
				syncSoundSettings();
			}
			if (event.kbd.keycode == Common::KEYCODE_s && !hasModifier) {
				ConfMan.setBool("sfx_mute", !ConfMan.getBool("sfx_mute"));
				syncSoundSettings();
			}
			if (event.kbd.keycode == Common::KEYCODE_F1 && !hasModifier) {
				if (_gameState->_inMenu) {
					playSoundWrong();
				} else
					showOptions();
			}

			if (event.kbd.flags & Common::KBD_ALT) {
				int slotNum = event.kbd.keycode - (event.kbd.keycode >= Common::KEYCODE_KP0 ? Common::KEYCODE_KP0 : Common::KEYCODE_0);
				if (slotNum >= 0 && slotNum <= 9 && canSaveGameStateCurrently()) {
					if (saveGame(slotNum, "")) {
						// ok
						Common::U32String buf = Common::U32String::format(_("Saved game in slot #%d "), slotNum);
						GUI::TimedMessageDialog dialog(buf, 1000);
						dialog.runModal();
					} else {
						Common::U32String buf = Common::U32String::format(_("Could not quick save into slot #%d"), slotNum);
						GUI::MessageDialog dialog(buf);
						dialog.runModal();

					}
				}
			}

			if (event.kbd.flags & Common::KBD_CTRL) {
				int slotNum = event.kbd.keycode - (event.kbd.keycode >= Common::KEYCODE_KP0 ? Common::KEYCODE_KP0 : Common::KEYCODE_0);
				if (slotNum >= 0 && slotNum <= 9 && canLoadGameStateCurrently()) {
					if (loadGame(slotNum)) {
						// ok
						Common::U32String buf = Common::U32String::format(_("Saved game #%d quick loaded"), slotNum);
						GUI::TimedMessageDialog dialog(buf, 1000);
						dialog.runModal();
					} else {
						const char *msg = _s("Could not quick load the saved game #%d");
						Common::U32String buf = Common::U32String::format(_(msg), slotNum);
						GUI::MessageDialog dialog(buf);
						warning(msg, slotNum);
						dialog.runModal();
					}
				}
			}
			break;
		default:
			break;
		}
	}

	if (!_gameState->_inConversation && !_gameState->_mouseHidden && !_gameState->_inInventory && !_gameState->_inMenu) {
		selectHotspot();
		clickEvent();
	}
}

void ToonEngine::enableTimer(int32 timerId) {
	_gameState->_timerEnabled[timerId] = true;
}
void ToonEngine::setTimer(int32 timerId, int32 timerWait) {
	_gameState->_timerTimeout[timerId] = getOldMilli() + timerWait * getTickLength();
	_gameState->_timerDelay[timerId] = timerWait;
}
void ToonEngine::disableTimer(int32 timerId) {
	_gameState->_timerEnabled[timerId] = false;
}
void ToonEngine::updateTimers() {
	for (int32 i = 0; i < 2; ++i) {
		if (_gameState->_timerEnabled[i]) {
			if (_gameState->_timerDelay[i] > -1 && getOldMilli() > _gameState->_timerTimeout[i]) {
				if (i == 0) {
					EMCState *status = &_scriptState[_currentScriptRegion];
					_script->init(status, &_scriptData);

					// setup registers
					status->regs[0] = _mouseX;
					status->regs[1] = _mouseY;
					status->regs[2] = 0;

					++_currentScriptRegion;

					_script->start(status, 7);
					while (_script->run(status))
						waitForScriptStep();

					--_currentScriptRegion;

					_gameState->_timerTimeout[i] = getOldMilli() + _gameState->_timerDelay[i] * getTickLength();

					return;

				}
			}
		}
	}
}

void ToonEngine::updateScrolling(bool force, int32 timeIncrement) {
	static int32 lastScrollOffset = 320;
	if (!_audioManager->voiceStillPlaying() && !_gameState->_currentScrollLock && (_drew->getFlag() & 1) == 0) {
		if (_drew->getFacing() & 3) {
			if (_drew->getFacing() <= 4)
				lastScrollOffset = 200;
			else
				lastScrollOffset = 440;
		}

		if (_gameState->_inCutaway || _gameState->_inInventory || _gameState->_inCloseUp)
			return;

		int32 desiredScrollValue = _drew->getX() - lastScrollOffset;

		if ((_gameState->_locations[_gameState->_currentScene]._flags & 0x80) == 0) {
			if (desiredScrollValue < 0)
				desiredScrollValue = 0;
			if (desiredScrollValue >= _currentPicture->getWidth() - TOON_SCREEN_WIDTH)
				desiredScrollValue = _currentPicture->getWidth() - TOON_SCREEN_WIDTH;

			if (force) {
				_gameState->_currentScrollValue = desiredScrollValue;
				return;
			} else {
				if (_gameState->_currentScrollValue < desiredScrollValue) {
					_gameState->_currentScrollValue += timeIncrement / 2;

					if (_gameState->_currentScrollValue > desiredScrollValue)
						_gameState->_currentScrollValue = desiredScrollValue;
				} else if (_gameState->_currentScrollValue > desiredScrollValue) {
					_gameState->_currentScrollValue -= timeIncrement / 2;

					if (_gameState->_currentScrollValue < desiredScrollValue)
						_gameState->_currentScrollValue = desiredScrollValue;
				}
			}
		}
	}
}

void ToonEngine::update(int32 timeIncrement) {
	// to make sure we're updating the game at 5fps at least
	if (timeIncrement > 200)
		timeIncrement = 200;

	updateAnimationSceneScripts(timeIncrement);
	updateCharacters(timeIncrement);
	updateTimer(timeIncrement);
	updateTimers();
	updateScrolling(false, timeIncrement);
	_audioManager->updateAmbientSFX();
	_animationManager->update(timeIncrement);
	_cursorAnimationInstance->update(timeIncrement);

	if (!_audioManager->voiceStillPlaying()) {
		_currentTextLine = 0;
		_currentTextLineId = -1;
	}
}

void ToonEngine::updateTimer(int32 timeIncrement) {
	if (_gameState->_gameTimer > 0) {
		debugC(0, 0xfff, "updateTimer(%d)", (int)timeIncrement);
		_gameState->_gameTimer -= timeIncrement;
		if (_gameState->_gameTimer < 0)
			_gameState->_gameTimer = 0;
	}
}

void ToonEngine::render() {

	if (_dirtyAll) {
		if (_gameState->_inCutaway)
			_currentCutaway->draw(*_mainSurface, 0, 0, 0, 0);
		else
			_currentPicture->draw(*_mainSurface, 0, 0, 0, 0);
		_dirtyRects.push_back(Common::Rect(0, 0, TOON_BACKBUFFER_WIDTH, TOON_BACKBUFFER_HEIGHT));
	} else {
		if (_gameState->_inCutaway)
			_currentCutaway->drawWithRectList(*_mainSurface, 0, 0, 0, 0, _dirtyRects);
		else
			_currentPicture->drawWithRectList(*_mainSurface, 0, 0, 0, 0, _dirtyRects);
	}

	clearDirtyRects();

	//_currentMask->drawMask(*_mainSurface, 0, 0, 0, 0);
	_animationManager->render();

	drawInfoLine();
	drawConversationLine();
	drawConversationIcons();
	drawSack();
	//drawPalette();						// used to debug the current palette
	//_drew->plotPath(*_mainSurface);		// used to debug path finding

#if 0
	if (_mouseX > 0 && _mouseX < 640 && _mouseY > 0 && _mouseY < 400) {
		Common::String test;
		test = Common::String::format("%d %d / mask %d layer %d z %d", _mouseX, _mouseY, getMask()->getData(_mouseX, _mouseY), getLayerAtPoint(_mouseX, _mouseY), getZAtPoint(_mouseX, _mouseY));

		int32 c = *(uint8 *)_mainSurface->getBasePtr(_mouseX, _mouseY);
		test = Common::String::format("%d %d / color id %d %d,%d,%d", _mouseX, _mouseY, c, _finalPalette[c * 3 + 0], _finalPalette[c * 3 + 1], _finalPalette[c * 3 + 2]);

		_fontRenderer->setFont(_fontToon);
		_fontRenderer->renderText(40, 150, test, 0);
	}
#endif

	if (_needPaletteFlush) {
		flushPalette(false);
		_needPaletteFlush = false;
	}

	if (_firstFrame) {
		copyToVirtualScreen(false);
		fadeIn(5);
		_firstFrame = false;
	} else {
		copyToVirtualScreen(true);
	}

	// add a little sleep here
	int32 newMillis = (int32)_system->getMillis();
	int32 sleepMs = 1; // Minimum delay to allow thread scheduling
	if ((newMillis - _lastRenderTime)  < _tickLength * 2)
		sleepMs = _tickLength * 2 - (newMillis - _lastRenderTime);
	assert(sleepMs >= 0);
	_system->delayMillis(sleepMs);
	_lastRenderTime = _system->getMillis();
}

void ToonEngine::doMagnifierEffect() {
	int32 posX = _mouseX + state()->_currentScrollValue - _cursorOffsetX;
	int32 posY = _mouseY - _cursorOffsetY - 2;

	Graphics::Surface &surface = *_mainSurface;

	// fast sqrt table lookup (values up to 144 only)
	static const byte intSqrt[] = {
		0, 1, 1, 1, 2, 2, 2, 2, 2, 3,
		3, 3, 3, 3, 3, 3, 4, 4, 4, 4,
		4, 4, 4, 4, 4, 5, 5, 5, 5, 5,
		5, 5, 5, 5, 5, 5, 6, 6, 6, 6,
		6, 6, 6, 6, 6, 6, 6, 6, 6, 7,
		7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
		7, 7, 7, 7, 8, 8, 8, 8, 8, 8,
		8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
		8, 9, 9, 9, 9, 9, 9, 9, 9, 9,
		9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
		10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
		10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
		10, 11, 11, 11, 11, 11, 11, 11, 11, 11,
		11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
		11, 11, 11, 11, 12
	};

	byte tempBuffer[25 * 25];
	for (int32 y = -12; y <= 12; ++y) {
		int32 cy = CLIP<int32>(posY + y, 0, TOON_BACKBUFFER_HEIGHT-1);
		for (int32 x = -12; x <= 12; ++x) {
			int32 cx = CLIP<int32>(posX + x, 0, TOON_BACKBUFFER_WIDTH-1);
			uint8 *curRow = (uint8 *)surface.getBasePtr(cx, cy);
			tempBuffer[(y + 12) * 25 + x + 12] = *curRow;
		}
	}

	for (int32 y = -12; y <= 12; ++y) {
		int32 cy = CLIP<int32>(posY + y, 0, TOON_BACKBUFFER_HEIGHT-1);
		for (int32 x = -12; x <= 12; ++x) {
			int32 dist = y * y + x * x;
			if (dist > 144)
				continue;
			int32 cx = CLIP<int32>(posX + x, 0, TOON_BACKBUFFER_WIDTH-1);
			uint8 *curRow = (uint8 *)surface.getBasePtr(cx, cy);
			int32 lerp = (512 + intSqrt[dist] * 256 / 12);
			*curRow = tempBuffer[(y * lerp / 1024 + 12) * 25 + x * lerp / 1024 + 12];
		}
	}
}

void ToonEngine::copyToVirtualScreen(bool updateScreen) {
	// render cursor last
	if (!_gameState->_mouseHidden) {
		if (_cursorAnimationInstance->getFrame() == 7 && _cursorAnimationInstance->getAnimation() == _cursorAnimation) // magnifier icon needs a special effect
			doMagnifierEffect();
		_cursorAnimationInstance->setPosition(_mouseX - 40 + state()->_currentScrollValue - _cursorOffsetX, _mouseY - 40 - _cursorOffsetY, 0, false);
		_cursorAnimationInstance->render();
	}

	// Handle dirty rects here
	static int32 lastScroll = 0;

	if (_dirtyAll || _gameState->_currentScrollValue != lastScroll) {
		// we have to refresh everything in case of scrolling.
		_system->copyRectToScreen((byte *)_mainSurface->getPixels() + state()->_currentScrollValue, TOON_BACKBUFFER_WIDTH, 0, 0, TOON_SCREEN_WIDTH, TOON_SCREEN_HEIGHT);
	} else {

		int32 offX = 0;
		for (uint i = 0; i < _oldDirtyRects.size(); ++i) {
			Common::Rect rect = _oldDirtyRects[i];
			rect.translate(-state()->_currentScrollValue, 0);
			offX = 0;
			if (rect.right <= 0)
				continue;
			if (rect.left < 0) {
				offX = -rect.left;
				rect.left = 0;
			}
			rect.clip(TOON_SCREEN_WIDTH, TOON_SCREEN_HEIGHT);
			if (rect.left >= 0 && rect.top >= 0 && rect.right - rect.left > 0 && rect.bottom - rect.top > 0) {
				_system->copyRectToScreen((byte *)_mainSurface->getBasePtr(_oldDirtyRects[i].left + offX, _oldDirtyRects[i].top), TOON_BACKBUFFER_WIDTH, rect.left , rect.top, rect.right - rect.left, rect.bottom - rect.top);
			}
		}

		for (uint i = 0; i < _dirtyRects.size(); ++i) {
			Common::Rect rect = _dirtyRects[i];
			rect.translate(-state()->_currentScrollValue, 0);
			offX = 0;
			if (rect.right <= 0)
				continue;
			if (rect.left < 0) {
				offX = -rect.left;
				rect.left = 0;
			}
			rect.clip(TOON_SCREEN_WIDTH, TOON_SCREEN_HEIGHT);
			if (rect.left >= 0 && rect.top >= 0 && rect.right - rect.left > 0 && rect.bottom - rect.top > 0) {
				_system->copyRectToScreen((byte *)_mainSurface->getBasePtr(_dirtyRects[i].left + offX, _dirtyRects[i].top), TOON_BACKBUFFER_WIDTH, rect.left , rect.top, rect.right - rect.left, rect.bottom - rect.top);
			}
		}
	}
	lastScroll = _gameState->_currentScrollValue;

	if (updateScreen) {
		_system->updateScreen();
		_shouldQuit = shouldQuit();	// update game quit flag - this shouldn't be called all the time, as it's a virtual function
	}
}

void ToonEngine::doFrame() {

	if (_gameState->_inInventory) {
		renderInventory();
	} else {
		render();

		int32 currentTimer = _system->getMillis();

		update(currentTimer - _oldTimer);
		_oldTimer = currentTimer;
		_oldTimer2 = currentTimer;
	}
	parseInput();
}

enum MainMenuSelections {
	MAINMENUHOTSPOT_NONE         = 0,
	MAINMENUHOTSPOT_START        = 1,
	MAINMENUHOTSPOT_INTRO        = 2,
	MAINMENUHOTSPOT_LOADGAME     = 3,
	MAINMENUHOTSPOT_HOTKEYS      = 4,
	MAINMENUHOTSPOT_CREDITS      = 5,
	MAINMENUHOTSPOT_QUIT         = 6,
	MAINMENUHOTSPOT_HOTKEYSCLOSE = 7
};

enum MainMenuMasks {
	MAINMENUMASK_BASE       = 1,
	MAINMENUMASK_HOTKEYS    = 2,
	MAINMENUMASK_EVERYWHERE = 3
};

enum OptionMenuSelections {
	OPTIONMENUHOTSPOT_NONE                  = 0,
	OPTIONMENUHOTSPOT_PLAY                  = 1,
	OPTIONMENUHOTSPOT_QUIT                  = 2,
	OPTIONMENUHOTSPOT_TEXT                  = 3,
	OPTIONMENUHOTSPOT_TEXTSPEED             = 4,
	OPTIONMENUHOTSPOT_VOLUMESFX             = 5,
	OPTIONMENUHOTSPOT_VOLUMESFXSLIDER       = 6,
	OPTIONMENUHOTSPOT_VOLUMEMUSIC           = 7,
	OPTIONMENUHOTSPOT_VOLUMEMUSICSLIDER     = 8,
	OPTIONMENUHOTSPOT_VOLUMEVOICE           = 9,
	OPTIONMENUHOTSPOT_VOLUMEVOICESLIDER     = 10,
	OPTIONMENUHOTSPOT_SPEAKERBUTTON         = 11,
	OPTIONMENUHOTSPOT_SPEAKERLEVER          = 12,
	OPTIONMENUHOTSPOT_VIDEO_MODE            = 13
};

enum OptionMenuMasks {
	OPTIONMENUMASK_EVERYWHERE = 1
};

struct MenuFile {
	int menuMask;
	int id;
	const char *animationFile;
	int animateOnFrame;
};

#define MAINMENU_ENTRYCOUNT 12
static const MenuFile mainMenuFiles[] = {
	{ MAINMENUMASK_BASE,       MAINMENUHOTSPOT_START,        "STARTBUT.CAF", 0 }, // "Start" button
	{ MAINMENUMASK_BASE,       MAINMENUHOTSPOT_INTRO,        "INTROBUT.CAF", 0 }, // "Intro" button
	{ MAINMENUMASK_BASE,       MAINMENUHOTSPOT_LOADGAME,     "LOADBUT.CAF",  0 }, // "Load Game" button
	{ MAINMENUMASK_BASE,       MAINMENUHOTSPOT_HOTKEYS,      "HOTBUT.CAF",   0 }, // "Hot Keys" button
	{ MAINMENUMASK_BASE,       MAINMENUHOTSPOT_CREDITS,      "CREDBUT.CAF",  0 }, // "Credits" button
	{ MAINMENUMASK_BASE,       MAINMENUHOTSPOT_QUIT,         "QUITBUT.CAF",  0 }, // "Quit" button
	{ MAINMENUMASK_BASE,       MAINMENUHOTSPOT_NONE,         "LEGALTXT.CAF", 0 }, // Legal Text

	{ MAINMENUMASK_EVERYWHERE, MAINMENUHOTSPOT_NONE,         "TOONGLOW.CAF", 6 }, // Clown glow
	{ MAINMENUMASK_EVERYWHERE, MAINMENUHOTSPOT_NONE,         "TOONSTRK.CAF", 6 }, // Toonstruck title
	{ MAINMENUMASK_EVERYWHERE, MAINMENUHOTSPOT_NONE,         "EYEGLOW.CAF",  4 }, // Clown eye glow
	{ MAINMENUMASK_EVERYWHERE, MAINMENUHOTSPOT_NONE,         "PROPHEAD.CAF", 4 }, // Clown propellor head
	{ MAINMENUMASK_HOTKEYS,    MAINMENUHOTSPOT_HOTKEYSCLOSE, "HOTKEYS.CAF",  0 }  // Hotkeys display - clicking on it will close hotkeys
};

#define OPTIONMENU_ENTRYCOUNT 27
static const MenuFile optionMenuFiles[] = {
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_PLAY,					"PLAYBUTN.CAF",	0 },	// "Play" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_QUIT,					"QUITBUTN.CAF",	0 },	// "Quit" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VIDEO_MODE,			"VIDMODE.CAF",	0 },	// "Video mode" slider
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_TEXTSPEED,			"TXTSPEED.CAF",	0 },	// "Text speed" slider
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_TEXT,					"TEXTDIAL.CAF",	0 },	// "Text" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VOLUMESFX,			"SFXBUTN.CAF",	0 },	// "SFX" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VOLUMESFXSLIDER,		"SFXSLDR.CAF",	0 },	// "SFX volume" slider
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VOLUMEVOICE,			"VOICEBTN.CAF",	0 },	// "Voice" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VOLUMEVOICESLIDER,	"VOICESLD.CAF",	0 },	// "Voice volume" slider
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VOLUMEMUSIC,			"MUSICBTN.CAF",	0 },	// "Music" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VOLUMEMUSICSLIDER,	"MUSICSLD.CAF",	0 },	// "Music volume" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_SPEAKERBUTTON,		"XTRABUTN.CAF",	0 },	// Right speaker button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_SPEAKERLEVER,			"XTRALEVR.CAF",	0 },	// Left speaker switch
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"ANTENNAL.CAF",	6 },	// Decorative animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"ANTENNAR.CAF",	6 },	// Decorative animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"BIGREDL.CAF",	6 },	// Decorative animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"BIGREDR.CAF",	6 },	// Decorative animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"GRIDLTEL.CAF",	6 },	// Decorative animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"GRIDLTER.CAF",	6 },	// Decorative animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"LSPEAKR.CAF",	0 },	// Left speaker animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"RSPEAKR.CAF",	0 },	// Right speaker animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"STARLITL.CAF",	6 },	// Decorative animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"STARLITR.CAF",	6 },	// Decorative animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"CHASE1.CAF",	6 },	// Decorative animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"CHASE2.CAF",	6 },	// Decorative animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"CHASE3.CAF",	6 },	// Decorative animation
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"CHASE4.CAF",	6 }		// Decorative animation
};

// English demo does not have most of the animations, but it has a random
// sparkle effect instead.
#define OPTIONMENU_ENTRYCOUNT_ENGLISH_DEMO 12
static const MenuFile optionMenuFilesEnglishDemo[] = {
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_PLAY,					"PLAYBUTN.CAF",	0 },	// "Play" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_QUIT,					"QUITBUTN.CAF",	0 },	// "Quit" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VIDEO_MODE,			"VIDMODE.CAF",	0 },	// "Video mode" slider
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_TEXTSPEED,			"TXTSPEED.CAF",	0 },	// "Text speed" slider
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_TEXT,					"TEXTDIAL.CAF",	0 },	// "Text" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VOLUMESFX,			"SFXBUTN.CAF",	0 },	// "SFX" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VOLUMESFXSLIDER,		"SFXSLDR.CAF",	0 },	// "SFX volume" slider
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VOLUMEVOICE,			"VOICEBTN.CAF",	0 },	// "Voice" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VOLUMEVOICESLIDER,	"VOICESLD.CAF",	0 },	// "Voice volume" slider
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VOLUMEMUSIC,			"MUSICBTN.CAF",	0 },	// "Music" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_VOLUMEMUSICSLIDER,	"MUSICSLD.CAF",	0 },	// "Music volume" button
	{ OPTIONMENUMASK_EVERYWHERE,	OPTIONMENUHOTSPOT_NONE,					"SPRKL.CAF",	6 }		// Sparkle animation
};

struct MenuEntry {
	int menuMask;
	int id;
	Animation *animation;
	Common::Rect rect;
	int animateOnFrame;
	int animateCurFrame;
	int activeFrame;
	int targetFrame;
	bool playOnce;
	bool handled;
};

bool ToonEngine::showOptions() {
	storePalette();
	fadeOut(5);
	Picture* optionPicture = new Picture(this);
	optionPicture->loadPicture("OPTIONS.CPS");
	optionPicture->setupPalette();
	flushPalette(true);

	int16 oldScrollValue = _gameState->_currentScrollValue;
	_gameState->_currentScrollValue = 0;

	bool oldMouseHidden = _gameState->_mouseHidden;
	_gameState->_mouseHidden = false;

	// English demo options menu has less animations and no SFX
	const int optionMenuEntryCount = _isEnglishDemo ? OPTIONMENU_ENTRYCOUNT_ENGLISH_DEMO : OPTIONMENU_ENTRYCOUNT;

	const MenuFile *optionMenuFilesPtr = _isEnglishDemo ? optionMenuFilesEnglishDemo : optionMenuFiles;
	MenuEntry *entries = new MenuEntry[optionMenuEntryCount];

	for (int entryNr = 0; entryNr < optionMenuEntryCount; ++entryNr) {
		entries[entryNr].menuMask = optionMenuFilesPtr[entryNr].menuMask;
		entries[entryNr].id = optionMenuFilesPtr[entryNr].id;
		entries[entryNr].animation = new Animation(this);
		entries[entryNr].animation->loadAnimation(optionMenuFilesPtr[entryNr].animationFile);
		if (entries[entryNr].id != OPTIONMENUHOTSPOT_NONE) {
			entries[entryNr].rect = entries[entryNr].animation->getRect();
			// Bug fix for short hotspot rectangle for the text speed slider
			// This bug is an original game bug.
			// NOTE If low resolution mode is supported in the future,
			//      this height increment should be adjusted accordingly
			if (entries[entryNr].id == OPTIONMENUHOTSPOT_TEXTSPEED)
				entries[entryNr].rect.bottom += 10;

			if (entries[entryNr].id == OPTIONMENUHOTSPOT_TEXT && !_isEnglishDemo) {
				// For the game proper we need to extend the rectangle for the TEXT hotspot
				// above and to the left and right, so that we can detect clicking on
				// each of the labels around the dial.
				// NOTE If low resolution mode is supported in the future,
				//      these rectangle dimensions should be adjusted accordingly
				entries[entryNr].rect.top -= 20;
				entries[entryNr].rect.left -= 65;
				entries[entryNr].rect.right += 65;
			}
		}
		entries[entryNr].animateOnFrame = optionMenuFilesPtr[entryNr].animateOnFrame;
		entries[entryNr].animateCurFrame = 0;
		entries[entryNr].activeFrame = 0;
		entries[entryNr].targetFrame = -1;
		entries[entryNr].playOnce = false;
		entries[entryNr].handled = false;
	}

	// Setting dial / option value in the game options menu
	entries[10].activeFrame = ConfMan.getInt("music_volume")  * (entries[10].animation->_numFrames - 1) / Audio::Mixer::kMaxMixerVolume;
	entries[8].activeFrame = ConfMan.getInt("speech_volume") * (entries[8].animation->_numFrames - 1) / Audio::Mixer::kMaxMixerVolume;
	entries[6].activeFrame = ConfMan.getInt("sfx_volume") * (entries[6].animation->_numFrames - 1) / Audio::Mixer::kMaxMixerVolume;

	entries[9].activeFrame = _audioManager->isMusicMuted() ? 0 : entries[9].animation->_numFrames - 1;
	entries[7].activeFrame = _audioManager->isVoiceMuted() ? 0 : entries[7].animation->_numFrames - 1;
	entries[5].activeFrame = _audioManager->isSfxMuted() ? 0 : entries[5].animation->_numFrames - 1;

	entries[3].activeFrame = _textSpeed * (entries[3].animation->_numFrames - 1) / 255;

	entries[2].activeFrame = entries[2].animation->_numFrames - 1;

	const int textOffFrame = _isEnglishDemo ? 0 : 4;
	const int textOnFrameFont1 = _isEnglishDemo ? 8 : 0;
	const int textOnFrameFont2 = 8;

	if (!_showConversationText) {
		entries[4].activeFrame = textOffFrame;
	} else if (_useAlternativeFont) {
		entries[4].activeFrame = textOnFrameFont2;
	} else {
		entries[4].activeFrame = textOnFrameFont1;
	}

	// Variables for the English demo sparkle animation.
	int sparkleDelay = 100;
	int sparklePosX = 0;
	int sparklePosY = 0;

	setCursor(0);

	int menuMask = OPTIONMENUMASK_EVERYWHERE;
	int ratioX = 0;
	int ratioY = 0;
	bool doExitMenu = false;
	bool exitGame = false;
	bool targetFrameExceeded = false;

	_gameState->_inMenu = true;
	dirtyAllScreen();
	_firstFrame = true;

	int32 oldMouseX = _mouseX;
	int32 oldMouseY = _mouseY;
	int32 oldMouseButton = _mouseButton;
	int targetVol, targetTextSpeed;
	Common::String chosenConfVolumeSoundKey;

	while (!doExitMenu) {

		int clickingOn = OPTIONMENUHOTSPOT_NONE;
		int clickingOnSprite = 0;
		bool clickRelease = false;

		while (!clickRelease) {

			if (_dirtyAll) {
				optionPicture->draw(*_mainSurface, 0, 0, 0, 0);
				addDirtyRect(0, 0, TOON_SCREEN_WIDTH, TOON_SCREEN_HEIGHT);
			} else {
				optionPicture->drawWithRectList(*_mainSurface, 0, 0, 0, 0, _dirtyRects);
			}
			clearDirtyRects();

			// Handle animations
			for (int entryNr = 0; entryNr < optionMenuEntryCount; ++entryNr) {
				if (entries[entryNr].menuMask & menuMask) {
					int animPosX = 0;
					int animPosY = 0;
					if (_isEnglishDemo && entryNr == 11) {
						// Special handling for the sparkles in the English demo.
						if (sparkleDelay > 0) {
							// Don't show the next sparkle until the delay has
							// counted down.
							--sparkleDelay;
							continue;
						} else if (entries[entryNr].animateCurFrame == 0 && entries[entryNr].activeFrame == 0) {
							// Start of a new sparkle animation. Generate a
							// random position on the screen.
							sparklePosX = randRange(0, 639 - entries[entryNr].animation->getWidth());
							sparklePosY = randRange(0, 399 - entries[entryNr].animation->getHeight());
						}
						animPosX = sparklePosX;
						animPosY = sparklePosY;
					}
					if (entries[entryNr].animateOnFrame) { // animateOnFrame is used to slow down an animation
						++entries[entryNr].animateCurFrame; // counter towards animateOnFrame
						if (entries[entryNr].animateOnFrame <= entries[entryNr].animateCurFrame) {

							if (entries[entryNr].targetFrame >= 0) {
								if (entries[entryNr].targetFrame >= entries[entryNr].animation->_numFrames) {
									entries[entryNr].targetFrame = entries[entryNr].animation->_numFrames - 1;
								}
								targetFrameExceeded = false;
								if (entries[entryNr].activeFrame <= entries[entryNr].targetFrame) {
									++entries[entryNr].activeFrame;
									if (entries[entryNr].activeFrame > entries[entryNr].targetFrame)
										targetFrameExceeded = true;
								} else if (entries[entryNr].activeFrame >= entries[entryNr].targetFrame) {
									--entries[entryNr].activeFrame;
									if (entries[entryNr].activeFrame < entries[entryNr].targetFrame)
										targetFrameExceeded = true;
								}

								if (targetFrameExceeded) {
									entries[entryNr].animateOnFrame = 0;
									entries[entryNr].activeFrame = entries[entryNr].targetFrame;
									entries[entryNr].targetFrame = -1;

									if (entries[entryNr].id == OPTIONMENUHOTSPOT_PLAY) { // PLAY BUTTON
										exitGame = false;
										doExitMenu = true;
									}

									if (entries[entryNr].id == OPTIONMENUHOTSPOT_QUIT) { // QUIT BUTTON
										exitGame = showQuitConfirmationDialogue();
										if (exitGame)  {
											doExitMenu = true;
										} else {
											entries[entryNr].activeFrame = 0;
										}
									}
								}
							} else {
								++entries[entryNr].activeFrame;
								if (!_isEnglishDemo && entries[entryNr].activeFrame == 3) {
									if (entryNr == 19)  {
										// The left (SPEECH test) horn has 7 frames.
										// Frame 3 works best to play the Burp Speech sound
										_audioManager->playVoice(316, true);
									} else if (entryNr == 20) {
										// The right (SFX test) horn has 7 frames.
										// Frame 3 works best to play the Bell SFX sound
										playSFX(-3, 128);
									}
								}
								if (entries[entryNr].activeFrame >= entries[entryNr].animation->_numFrames) {
									entries[entryNr].activeFrame = 0;
									if (_isEnglishDemo && entryNr == 11) {
										// Sparkle animation has finished. Generate
										// a random delay until the next sparkle.
										sparkleDelay = randRange(0, 100);
									}
									if (entries[entryNr].playOnce) {
										entries[entryNr].animateOnFrame = 0;
										entries[entryNr].playOnce = false;
									}
								}
							}
							entries[entryNr].animateCurFrame = 0;
						}
					}
					entries[entryNr].animation->drawFrame(*_mainSurface, entries[entryNr].activeFrame, animPosX, animPosY);
				}
			}

			oldMouseX = _mouseX;
			oldMouseY = _mouseY;
			oldMouseButton = _mouseButton;

			if (_shouldQuit || doExitMenu) {
				clickingOn = OPTIONMENUHOTSPOT_NONE;
				clickRelease = true;
				doExitMenu = true;
				// Prevent holding left mouse button down to be detected
				// as a new click when returning from menu
				_lastMouseButton = _mouseButton;
			} else {
				// update mouse clicking state and handle hotkeys
				parseInput();

				copyToVirtualScreen(true);
				if (_firstFrame) {
					_firstFrame = false;
					fadeIn(5);
				}
				_system->delayMillis(17);

				// animations related with handling hotkey commands
				if (entries[4].animateOnFrame == 0) {
					if (!_showConversationText && entries[4].activeFrame != textOffFrame) {
						entries[4].targetFrame = textOffFrame;
						entries[4].animateOnFrame = 1;
						entries[4].playOnce = true;
					} else if (_showConversationText
					           && (entries[4].activeFrame != textOnFrameFont1
							       && (_isEnglishDemo || (!_isEnglishDemo && entries[4].activeFrame != textOnFrameFont2)))) {
						if (!_isEnglishDemo) {
							entries[4].targetFrame = ConfMan.getBool("alternative_font") ? textOnFrameFont2 : textOnFrameFont1;
						} else
							entries[4].targetFrame = textOnFrameFont1;
						entries[4].animateOnFrame = 1;
						entries[4].playOnce = true;
					}
					if (!_isEnglishDemo && entries[4].animateOnFrame == 1) {
						playSFX(-9, 128);
					}
				}

				if (entries[9].animateOnFrame == 0) {
					if (!_audioManager->isMusicMuted() && entries[9].activeFrame != entries[9].animation->_numFrames - 1) {
						entries[9].targetFrame = entries[9].animation->_numFrames - 1;
						entries[9].animateOnFrame = 1;
						entries[9].playOnce = true;
					} else if (_audioManager->isMusicMuted() && entries[9].activeFrame != 0) {
						entries[9].targetFrame = 0;
						entries[9].animateOnFrame = 1;
						entries[9].playOnce = true;
					}
					if (!_isEnglishDemo && entries[9].animateOnFrame == 1) {
						playSFX(-7, 128);
					}
				}

				if (entries[7].animateOnFrame == 0) {
					if (!_audioManager->isVoiceMuted() && entries[7].activeFrame != entries[7].animation->_numFrames - 1) {
						entries[7].targetFrame = entries[7].animation->_numFrames - 1;
						entries[7].animateOnFrame = 1;
						entries[7].playOnce = true;
					} else if (_audioManager->isVoiceMuted() && entries[7].activeFrame != 0) {
						entries[7].targetFrame = 0;
						entries[7].animateOnFrame = 1;
						entries[7].playOnce = true;
					}
					if (!_isEnglishDemo && entries[7].animateOnFrame == 1) {
						playSFX(-7, 128);
					}
				}

				if (entries[5].animateOnFrame == 0) {
					if (!_audioManager->isSfxMuted() && entries[5].activeFrame != entries[5].animation->_numFrames - 1) {
						entries[5].targetFrame = entries[5].animation->_numFrames - 1;
						entries[5].animateOnFrame = 1;
						entries[5].playOnce = true;
					} else if (_audioManager->isSfxMuted() && entries[5].activeFrame != 0) {
						entries[5].targetFrame = 0;
						entries[5].animateOnFrame = 1;
						entries[5].playOnce = true;
					}
					if (!_isEnglishDemo && entries[5].animateOnFrame == 1) {
						playSFX(-7, 128);
					}
				}

				// Avoid unnecessary checks and actions if mouse has not moved or changed status
				if (oldMouseButton != _mouseButton
				    || ((_mouseButton & 1)
				        && (oldMouseX != _mouseX || oldMouseY != _mouseY))) {
					if (_mouseButton & 1) {
						// left mouse button pressed
						for (int entryNr = 0; entryNr < optionMenuEntryCount; ++entryNr) {
							if (entries[entryNr].menuMask & menuMask
							    && entries[entryNr].id != OPTIONMENUHOTSPOT_NONE
							    && entries[entryNr].rect.contains(_mouseX, _mouseY)
							    && ((clickingOn == OPTIONMENUHOTSPOT_NONE && !(oldMouseButton & 1))
							        || (clickingOn == entries[entryNr].id && !entries[entryNr].handled))) {
								clickingOn = entries[entryNr].id;
								clickingOnSprite = entryNr;
								// Note, due to how rect.contains() is implemented,
								// the difference (_mouseX - entries[entryNr].rect.left)
								// will always be lower than entries[entryNr].rect.width()
								// and thus ratioX will always be lower than 256.
								// This is intentional.
								ratioX = (_mouseX - entries[entryNr].rect.left) * 256 / entries[entryNr].rect.width();
								ratioY = (_mouseY - entries[entryNr].rect.top) * 256 / entries[entryNr].rect.height();
								break;
							}
						}
					} else if (clickingOn != OPTIONMENUHOTSPOT_NONE) {
						// left mouse button released/not pushed down
						clickRelease = true;
						clickingOn = OPTIONMENUHOTSPOT_NONE;
						entries[clickingOnSprite].handled = false;
					}

					// handle sliders
					switch (clickingOn) {
					case OPTIONMENUHOTSPOT_VOLUMEMUSICSLIDER:
						// fall through
					case OPTIONMENUHOTSPOT_VOLUMEVOICESLIDER:
						// fall through
					case OPTIONMENUHOTSPOT_VOLUMESFXSLIDER:
						entries[clickingOnSprite].targetFrame = ratioX * (entries[clickingOnSprite].animation->_numFrames) / 256;
						entries[clickingOnSprite].animateOnFrame = 1;
						entries[clickingOnSprite].playOnce = true;

						targetVol = entries[clickingOnSprite].targetFrame * Audio::Mixer::kMaxMixerVolume / (entries[clickingOnSprite].animation->_numFrames - 1);
						// Since we use integer division, find a value for targetVol that will produce the same targetFrame we have calculated
						// We need this value for setting the proper frame for the slider needle indicator, when resuming the Options menu.
						while (entries[clickingOnSprite].targetFrame > targetVol * (entries[clickingOnSprite].animation->_numFrames - 1) / Audio::Mixer::kMaxMixerVolume)
							++targetVol;

						if (clickingOn == OPTIONMENUHOTSPOT_VOLUMEMUSICSLIDER) {
							chosenConfVolumeSoundKey = "music_volume";
						} else if (clickingOn == OPTIONMENUHOTSPOT_VOLUMEVOICESLIDER) {
							chosenConfVolumeSoundKey = "speech_volume";
						} else {
							chosenConfVolumeSoundKey = "sfx_volume";
						}
						if (ConfMan.getInt(chosenConfVolumeSoundKey) != targetVol) {
							ConfMan.setInt(chosenConfVolumeSoundKey, targetVol);
							syncSoundSettings();
						}
						break;

					case OPTIONMENUHOTSPOT_TEXTSPEED:
						entries[clickingOnSprite].targetFrame = ratioX * (entries[clickingOnSprite].animation->_numFrames) / 256;
						entries[clickingOnSprite].animateOnFrame = 1;
						entries[clickingOnSprite].playOnce = true;

						targetTextSpeed = 0;
						targetTextSpeed = entries[clickingOnSprite].targetFrame * 255 / (entries[clickingOnSprite].animation->_numFrames - 1);
						// Since we use integer division, find a value for _textSpeed that will produce the same targetFrame we have calculated
						// We need this value for setting the proper frame for the slider needle indicator, when resuming the Options menu.
						while (entries[clickingOnSprite].targetFrame > targetTextSpeed * (entries[clickingOnSprite].animation->_numFrames - 1) / 255)
							++targetTextSpeed;

						if (ConfMan.getInt("talkspeed") != targetTextSpeed) {
							ConfMan.setInt("talkspeed", targetTextSpeed);
							syncSoundSettings();
						}
						break;

					default:
						break;
					}

					// handle buttons
					if (clickingOn != OPTIONMENUHOTSPOT_NONE && !entries[clickingOnSprite].handled) {
						switch (clickingOn) {
						case OPTIONMENUHOTSPOT_PLAY:
							// fall through
						case OPTIONMENUHOTSPOT_QUIT:
							entries[clickingOnSprite].handled = true;
							entries[clickingOnSprite].targetFrame = entries[clickingOnSprite].animation->_numFrames - 1;
							entries[clickingOnSprite].animateOnFrame = 1;
							entries[clickingOnSprite].playOnce = true;
							if (!_isEnglishDemo) {
								if (clickingOn == OPTIONMENUHOTSPOT_PLAY)
									playSFX(-7, 128);
								else
									playSFX(-8, 128);
							}
							break;

						case OPTIONMENUHOTSPOT_VOLUMEMUSIC:
							// fall through
						case OPTIONMENUHOTSPOT_VOLUMEVOICE:
							// fall through
						case OPTIONMENUHOTSPOT_VOLUMESFX:
							entries[clickingOnSprite].handled = true;
							if (entries[clickingOnSprite].activeFrame != entries[clickingOnSprite].animation->_numFrames - 1) {
								entries[clickingOnSprite].targetFrame = entries[clickingOnSprite].animation->_numFrames - 1;
								entries[clickingOnSprite].animateOnFrame = 1;
								entries[clickingOnSprite].playOnce = true;
								if (clickingOn == OPTIONMENUHOTSPOT_VOLUMEMUSIC)
									ConfMan.setBool("music_mute", false);
								else if (clickingOn == OPTIONMENUHOTSPOT_VOLUMEVOICE)
									ConfMan.setBool("speech_mute", false);
								else
									ConfMan.setBool("sfx_mute", false);
								syncSoundSettings();
							} else {
								entries[clickingOnSprite].targetFrame = 0;
								entries[clickingOnSprite].animateOnFrame = 1;
								entries[clickingOnSprite].playOnce = true;
								if (clickingOn == OPTIONMENUHOTSPOT_VOLUMEMUSIC) {
									ConfMan.setBool("music_mute", true);
								} else if (clickingOn == OPTIONMENUHOTSPOT_VOLUMEVOICE) {
									ConfMan.setBool("speech_mute", true);
								} else
									ConfMan.setBool("sfx_mute", true);
								syncSoundSettings();
							}

							if (!_isEnglishDemo)
								playSFX(-7, 128);
							break;

						case OPTIONMENUHOTSPOT_SPEAKERBUTTON:
							entries[clickingOnSprite].handled = true;
							entries[clickingOnSprite].animateOnFrame = 4;
							entries[clickingOnSprite].playOnce = true;

							entries[19].animateOnFrame = 4;
							entries[19].playOnce = true;

							if (!_isEnglishDemo)
								playSFX(-10, 128);
							break;

						case OPTIONMENUHOTSPOT_SPEAKERLEVER:
							entries[clickingOnSprite].handled = true;
							// Speaker lever animation has 2 frames (on and off position).
							// Set the activeFrame to the other position than the current one.
							entries[clickingOnSprite].activeFrame = entries[clickingOnSprite].activeFrame ? 0 : 1;
							if (entries[clickingOnSprite].activeFrame == 1) {
								entries[20].animateOnFrame = 4;
								entries[20].playOnce = false;
							} else {
								entries[20].playOnce = true;
							}
							if (!_isEnglishDemo)
								playSFX(-10, 128);
							break;

						case OPTIONMENUHOTSPOT_TEXT:
							entries[clickingOnSprite].handled = true;
							if (!_isEnglishDemo) {
								if ((ratioY <= 151 && ratioX >= 88 && ratioX <= 169)
								    || (ratioY > 151 && ratioX >= 122 && ratioX <= 145) ) {
									ConfMan.setBool("subtitles", false);
									syncSoundSettings();
									entries[clickingOnSprite].targetFrame = 4;
									entries[clickingOnSprite].animateOnFrame = 1;
									entries[clickingOnSprite].playOnce = true;
								} else if (ratioY > 151 && ratioX > 145) {
									ConfMan.setBool("subtitles", true);
									ConfMan.setBool("alternative_font", true);
									syncSoundSettings();
									entries[clickingOnSprite].targetFrame = 8;
									entries[clickingOnSprite].animateOnFrame = 1;
									entries[clickingOnSprite].playOnce = true;
								} else if (ratioY > 151 && ratioX < 122) {
									ConfMan.setBool("subtitles", true);
									ConfMan.setBool("alternative_font", false);
									syncSoundSettings();
									entries[clickingOnSprite].targetFrame = 0;
									entries[clickingOnSprite].animateOnFrame = 1;
									entries[clickingOnSprite].playOnce = true;
								}
								if (entries[clickingOnSprite].animateOnFrame == 1)
									playSFX(-9, 128);
							} else {
								// In the demo, the behavior is different:
								// Clicking anywhere in the Text Dial hotspot
								// toggles between "Text Off" and "Text On"
								switch (entries[clickingOnSprite].activeFrame) {
								case 0:
									ConfMan.setBool("subtitles", true);
									syncSoundSettings();
									entries[clickingOnSprite].targetFrame = 8;
									entries[clickingOnSprite].animateOnFrame = 1;
									entries[clickingOnSprite].playOnce = true;
									break;

								case 8:
									ConfMan.setBool("subtitles", false);
									syncSoundSettings();
									entries[clickingOnSprite].targetFrame = 0;
									entries[clickingOnSprite].animateOnFrame = 1;
									entries[clickingOnSprite].playOnce = true;
									break;

								default:
									break;
								}
							}
							break;

						// don't allow change to video mode
						case OPTIONMENUHOTSPOT_VIDEO_MODE:
							entries[clickingOnSprite].handled = true;
							playSoundWrong();
							break;

						default:
							break;
						}
					}
				}
			}
		}
	}

	fadeOut(5);
	_gameState->_mouseHidden = oldMouseHidden;
	_gameState->_inMenu = false;
	_firstFrame = true;
	_gameState->_currentScrollValue = oldScrollValue;

	restorePalette();
	dirtyAllScreen();

	for (int entryNr = 0; entryNr < optionMenuEntryCount; ++entryNr)
		delete entries[entryNr].animation;
	delete[] entries;

	delete optionPicture;

	if (!_shouldQuit && exitGame) {
		_shouldQuit = exitGame;
	}
	return exitGame;
}

bool ToonEngine::showMainMenu(bool &loadedGame) {
	Picture *mainmenuPicture = new Picture(this);
	mainmenuPicture->loadPicture("TITLESCR.CPS");
	mainmenuPicture->setupPalette();
	flushPalette(false);

	MenuEntry entries[MAINMENU_ENTRYCOUNT];

	for (int entryNr = 0; entryNr < MAINMENU_ENTRYCOUNT; ++entryNr) {
		entries[entryNr].menuMask = mainMenuFiles[entryNr].menuMask;
		entries[entryNr].id = mainMenuFiles[entryNr].id;
		entries[entryNr].animation = new Animation(this);
		entries[entryNr].animation->loadAnimation(mainMenuFiles[entryNr].animationFile);
		if (entries[entryNr].id != MAINMENUHOTSPOT_NONE) {
			entries[entryNr].rect = entries[entryNr].animation->getRect();
			if (entries[entryNr].id == MAINMENUHOTSPOT_HOTKEYSCLOSE) {
				// In the original game, clicking anywhere on the
				// hotspots' screen will return the user to the main menu
				entries[entryNr].rect.top = 0;
				entries[entryNr].rect.left = 0;
				entries[entryNr].rect.right = TOON_SCREEN_WIDTH;
				entries[entryNr].rect.bottom = TOON_SCREEN_HEIGHT;
			}
		}
		entries[entryNr].animateOnFrame = mainMenuFiles[entryNr].animateOnFrame;
		entries[entryNr].animateCurFrame = 0;
		entries[entryNr].activeFrame = 0;
		entries[entryNr].handled = false;
	}

	setCursor(0);

	bool doExitMenu = false;
	bool exitGame = false;
	int menuMask = MAINMENUMASK_BASE;
	bool musicPlaying = false;
	int musicPlayingChannel = -1;
	int32 oldMouseButton = _mouseButton;

	_gameState->_inMenu = true;
	dirtyAllScreen();

	while (!doExitMenu) {
		int clickingOn = MAINMENUHOTSPOT_NONE;
		int clickingOnSprite = 0;
		bool resetHotspotLoop = false;
		bool clickEarlyRelease = false;

		while (!resetHotspotLoop) {
			if (!musicPlaying) {
				musicPlayingChannel = _audioManager->playMusic("", "BR091013");
				musicPlaying = musicPlayingChannel >= 0;
			}

			if (_dirtyAll) {
				mainmenuPicture->draw(*_mainSurface, 0, 0, 0, 0);
				addDirtyRect(0, 0, TOON_SCREEN_WIDTH, TOON_SCREEN_HEIGHT);
			} else {
				mainmenuPicture->drawWithRectList(*_mainSurface, 0, 0, 0, 0, _dirtyRects);
			}

			clearDirtyRects();

			// Handle animations
			for (int entryNr = 0; entryNr < MAINMENU_ENTRYCOUNT; ++entryNr) {
				if (entries[entryNr].menuMask & menuMask) {
					if (entries[entryNr].animateOnFrame) {
						++entries[entryNr].animateCurFrame;
						if (entries[entryNr].animateOnFrame <= entries[entryNr].animateCurFrame) {
							++entries[entryNr].activeFrame;
							if (entries[entryNr].activeFrame >= entries[entryNr].animation->_numFrames)
								entries[entryNr].activeFrame = 0;
							entries[entryNr].animateCurFrame = 0;
						}
					}
					entries[entryNr].animation->drawFrame(*_mainSurface, entries[entryNr].activeFrame, 0, 0);
				}
			}

			if (_needPaletteFlush) {
				flushPalette(false);
				_needPaletteFlush = false;
			}

			oldMouseButton = _mouseButton;

			if (_shouldQuit || doExitMenu) {
				clickingOn = MAINMENUHOTSPOT_NONE;
				resetHotspotLoop = true;
				doExitMenu = true;
				// Prevent holding left mouse button down to be detected
				// as a new click when returning from menu
				_lastMouseButton = _mouseButton;
			} else {
				// update mouse clicking state and handle hotkeys
				// TODO The code handling menu button presses could be further simplified
				//      since we expect that a clicked button (on left-mouse-down) will go through
				//      two steps of handling (step 1: pressed state and sound, step 2: doing the relevant task),
				//      and the sequence of these two steps should not be broken once it's started.
				parseInput();

				copyToVirtualScreen(true);
				_system->delayMillis(17);

				if (_mouseButton & 1) {
					// left mouse button pushed down
					for (int entryNr = 0; entryNr < MAINMENU_ENTRYCOUNT; ++entryNr) {
						if ((entries[entryNr].menuMask & menuMask)
						    && entries[entryNr].id != MAINMENUHOTSPOT_NONE
						    && entries[entryNr].rect.contains(_mouseX, _mouseY)
						    && (clickingOn == MAINMENUHOTSPOT_NONE && !(oldMouseButton & 1))) {
							clickingOn = entries[entryNr].id;
							clickingOnSprite = entryNr;
							break;
						}
					}
				} else if (clickingOn != MAINMENUHOTSPOT_NONE) {
					// left mouse button released/not pushed down
					if (!entries[clickingOnSprite].handled)
						clickEarlyRelease = true;
					else {
						resetHotspotLoop = true;
						clickingOn = MAINMENUHOTSPOT_NONE;
						entries[clickingOnSprite].handled = false;
					}
				}

				// handle buttons
				if (clickingOn != MAINMENUHOTSPOT_NONE && !entries[clickingOnSprite].handled) {
					// NOTE "MAINMENUHOTSPOT_HOTKEYSCLOSE" does not have two frames
					if (entries[clickingOnSprite].animation->_numFrames > 1 && entries[clickingOnSprite].activeFrame == 0) {
						// First show the button as clicked
						entries[clickingOnSprite].activeFrame = 1;

						// Use click sfx sound only for:
						// Start game, Load Game and Hotkeys menu (but not going back from it)
						// Use special sfx sound for quit
						switch (clickingOn) {
						case MAINMENUHOTSPOT_HOTKEYS:
							// fall through
						case MAINMENUHOTSPOT_START:
							// fall through
						case MAINMENUHOTSPOT_LOADGAME:
							// fall through
							playSFX(-9, 128);
							break;

						case MAINMENUHOTSPOT_QUIT:
							playSFX(-8, 128);
							break;

						default:
							break;
						}
					} else {
						entries[clickingOnSprite].handled = true;
						switch (entries[clickingOnSprite].id) {
						case MAINMENUHOTSPOT_HOTKEYS:
							// fall through
						case MAINMENUHOTSPOT_HOTKEYSCLOSE:
							menuMask = (clickingOn == MAINMENUHOTSPOT_HOTKEYS)? MAINMENUMASK_HOTKEYS : MAINMENUMASK_BASE;
							entries[clickingOnSprite].activeFrame = 0;
							break;

						case MAINMENUHOTSPOT_START:
							// Start game (actually exit main menu)
							clickingOn = MAINMENUHOTSPOT_NONE;
							resetHotspotLoop = true;
							loadedGame = false;
							doExitMenu = true;
							break;

						case MAINMENUHOTSPOT_LOADGAME:

							doExitMenu = loadGame(-1);
							loadedGame = doExitMenu;
							if (loadedGame) {
								clickingOn = MAINMENUHOTSPOT_NONE;
								resetHotspotLoop = true;
							} else {
								entries[clickingOnSprite].activeFrame = 0;
							}
							exitGame = false;
							break;

						case MAINMENUHOTSPOT_INTRO:
							// fall through
						case MAINMENUHOTSPOT_CREDITS:
							if (musicPlaying) {
								//stop music
								_audioManager->stopMusicChannel(musicPlayingChannel, false);
								musicPlaying = false;
							}
							if (clickingOn == MAINMENUHOTSPOT_INTRO) {
								// Play intro movies
								getMoviePlayer()->play("209_1M.SMK", 0x10);
								getMoviePlayer()->play("209_2M.SMK", 0x10);
								getMoviePlayer()->play("209_3M.SMK", 0x10);
							} else {
								// Play credits movie
								getMoviePlayer()->play("CREDITS.SMK", 0x0);
							}
							entries[clickingOnSprite].activeFrame = 0;
							break;

						case MAINMENUHOTSPOT_QUIT:
							exitGame = showQuitConfirmationDialogue();
							if (exitGame)  {
								clickingOn = MAINMENUHOTSPOT_NONE;
								resetHotspotLoop = true;
								doExitMenu = true;
							} else {
								entries[clickingOnSprite].activeFrame = 0;
							}
							break;

						default:
							break;
						}

						if (clickEarlyRelease) {
							resetHotspotLoop = true;
						}
					}
				}
			}
		}

		if (musicPlaying && doExitMenu) {
			//stop music
			_audioManager->stopMusicChannel(musicPlayingChannel, false);
			musicPlaying = false;
		}
	}

	_gameState->_inMenu = false;

	for (int entryNr = 0; entryNr < MAINMENU_ENTRYCOUNT; ++entryNr)
		delete entries[entryNr].animation;
	delete mainmenuPicture;

	if (!_shouldQuit && exitGame) {
		 _shouldQuit = exitGame;
	}
	return !exitGame;
}

bool ToonEngine::showQuitConfirmationDialogue() {
	// In the original game this dialogue prompt was:
	// "Are you sure you want to exit? (Y/N)"
	// See: devtools\create_toon\staticdata.h
	// We could allow create_toon to include this text
	// and all its variations for the game's localizations in toon.dat,
	// especially if we implement a native game dialogue prompt.
	// But using ScummVM's Message Dialogue works just as well,
	// and it requires a mouse click for yes/no selection
	// instead of a keyboard key-press (which would also be dependent on the
	// text variant of the yes/no option).
	GUI::MessageDialog dialog(_("Are you sure you want to exit?"), _("Yes"), _("No"));
	return (dialog.runModal() == GUI::kMessageOK);
}

Common::Error ToonEngine::run() {

	if (!loadToonDat())
		return Common::kUnknownError;

	initGraphics(TOON_SCREEN_WIDTH, TOON_SCREEN_HEIGHT);
	init();

	// do we need to load directly a game?
	bool loadedGame = false;
	int32 slot = ConfMan.getInt("save_slot");
	if (slot > -1) {
		loadedGame = loadGame(slot);
	}

	if (!loadedGame) {

		// play producer intro
		// not all demo versions include the logo video
		getMoviePlayer()->play("VIELOGOM.SMK", _isDemo ? 0x12 : 0x10);

		// show mainmenu
		// the demo does not have a menu and starts a new game right away
		if (!_isDemo && !showMainMenu(loadedGame)) {
			return Common::kNoError;
		}
	}

	//loadScene(17);
	//loadScene(37);
	if (!loadedGame) {
		newGame();
	}

	while (!_shouldQuit && _gameState->_currentScene != -1)
		doFrame();
	return Common::kNoError;
}

ToonEngine::ToonEngine(OSystem *syst, const ADGameDescription *gameDescription)
	: Engine(syst), _gameDescription(gameDescription),
	_language(gameDescription->language), _rnd("toon") {
	_tickLength = 16;
	_currentPicture = nullptr;
	_inventoryPicture = nullptr;
	_currentMask = nullptr;
	_showConversationText = true;
	_textSpeed = 60;
	_useAlternativeFont = false;
	_isDemo = _gameDescription->flags & ADGF_DEMO;
	_isEnglishDemo = _isDemo && _gameDescription->language == Common::EN_ANY;

	_resources = nullptr;
	_animationManager = nullptr;
	_moviePlayer = nullptr;
	_mainSurface = nullptr;

	_finalPalette = nullptr;
	_backupPalette = nullptr;
	_additionalPalette1 = nullptr;
	_additionalPalette2 = nullptr;
	_additionalPalette2Present = false;
	_cutawayPalette = nullptr;
	_universalPalette = nullptr;
	_fluxPalette = nullptr;

	_roomScaleData = nullptr;
	_shadowLUT = nullptr;

	_conversationData = nullptr;

	_fontRenderer = nullptr;
	_fontToon = nullptr;
	_fontEZ = nullptr;
	_hotspots = nullptr;
	_genericTexts = nullptr;
	_roomTexts = nullptr;
	_script_func = nullptr;
	_script = nullptr;

	_mouseX = 0;
	_mouseY = 0;
	_mouseButton = 0;
	_lastMouseButton = 0;

	_saveBufferStream = nullptr;

	_pathFinding = nullptr;
	setDebugger(new ToonConsole(this));

	_cursorAnimation = nullptr;
	_cursorAnimationInstance = nullptr;
	_dialogIcons = nullptr;
	_inventoryIcons = nullptr;
	_inventoryIconSlots = nullptr;
	_genericTexts = nullptr;
	_audioManager = nullptr;
	_gameState = nullptr;

	_locationDirNotVisited = nullptr;
	_locationDirVisited = nullptr;
	_specialInfoLine = nullptr;

	for (int i = 0; i < 64; ++i) {
		_sceneAnimations[i]._active = false;
	}

	for (int i = 0; i < 32; ++i) {
		_characters[i] = nullptr;
	}

	memset(&_scriptData, 0, sizeof(EMCData));

	switch (_language) {
	case Common::EN_GRB:
	case Common::EN_USA:
	case Common::EN_ANY:
		_gameVariant = 0;
		break;
	case Common::FR_FRA:
		_gameVariant = 1;
		break;
	case Common::DE_DEU:
		_gameVariant = 2;
		break;
	case Common::RU_RUS:
		_gameVariant = 3;
		break;
	case Common::ES_ESP:
		_gameVariant = 4;
		break;
	default:
		// 0 - english
		_gameVariant = 0;
		break;
	}

	for (int i = 0; i < 64; ++i) {
		_sceneAnimationScripts[i]._lastTimer = 0;
		_sceneAnimationScripts[i]._frozen = false;
		_sceneAnimationScripts[i]._frozenForConversation = false;
		_sceneAnimationScripts[i]._active = false;
	}

	_lastProcessedSceneScript = 0;
	_animationSceneScriptRunFlag = false;
	_updatingSceneScriptRunFlag = false;
	_dirtyAll = false;
	_cursorOffsetX = 0;
	_cursorOffsetY = 0;
	_currentTextLine = 0;
	_currentTextLineId = 0;
	_currentTextLineX = 0;
	_currentTextLineY = 0;
	_currentTextLineCharacterId = -1;
	_oldScrollValue = 0;
	_drew = nullptr;
	_flux = nullptr;
	_currentHotspotItem = 0;
	_shouldQuit = false;
	_scriptStep = 0;
	_oldTimer = 0;
	_oldTimer2 = 0;
	_lastRenderTime = 0;
	_firstFrame = false;
	_needPaletteFlush = true;

	_numVariant = 0;
	_currentCutaway = nullptr;
	for (int i = 0; i < 4; ++i) {
		_scriptState[i].ip = nullptr;
		_scriptState[i].dataPtr = nullptr;
		_scriptState[i].retValue = 0;
		_scriptState[i].bp = 0;
		_scriptState[i].sp = 0;
		_scriptState[i].running = false;
	}
	_currentScriptRegion = 0;
	_currentFont = nullptr;
}

ToonEngine::~ToonEngine() {
	delete _currentPicture;
	delete _currentCutaway;
	delete _currentMask;
	delete _inventoryPicture;

	delete _resources;
	delete _animationManager;
	delete _moviePlayer;

	if (_mainSurface) {
		_mainSurface->free();
		delete _mainSurface;
	}

	delete[] _finalPalette;
	delete[] _backupPalette;
	delete[] _additionalPalette1;
	delete[] _additionalPalette2;
	delete[] _cutawayPalette;
	delete[] _universalPalette;
	delete[] _fluxPalette;

	delete[] _roomScaleData;
	delete[] _shadowLUT;

	delete[] _conversationData;

	delete _fontRenderer;
	delete _fontToon;
	delete _fontEZ;
	delete _hotspots;
	delete _genericTexts;
	delete _roomTexts;
	delete _script_func;

	_script->unload(&_scriptData);
	delete _script;

	delete _saveBufferStream;

	delete _pathFinding;

	for (int32 i = 0; i < 64; ++i) {
		if (_sceneAnimations[i]._active) {
			// see if one character shares this instance
			for (int32 c = 0; c < 32; ++c) {
				if (_characters[c] != nullptr && _characters[c]->getAnimationInstance() == _sceneAnimations[i]._animInstance) {
					_characters[c]->setAnimationInstance(0);
				}
			}
			delete _sceneAnimations[i]._originalAnimInstance;
			delete _sceneAnimations[i]._animation;
		}
	}

	for (int32 i = 0; i < 32; ++i)
		delete _characters[i];

	delete _cursorAnimation;
	delete _cursorAnimationInstance;
	delete _dialogIcons;
	delete _inventoryIcons;
	delete _inventoryIconSlots;
	//delete _genericTexts;
	delete _audioManager;
	delete _gameState;

	unloadToonDat();
}

void ToonEngine::flushPalette(bool deferFlushToNextRender) {

	if (deferFlushToNextRender) {
		_needPaletteFlush = true;
		return;
	}
	_needPaletteFlush = false;
	_system->getPaletteManager()->setPalette(_finalPalette, 0, 256);
}
void ToonEngine::setPaletteEntries(uint8 *palette, int32 offset, int32 num) {
	memcpy(_finalPalette + offset * 3, palette, num * 3);
	flushPalette();
}

void ToonEngine::simpleUpdate(bool waitCharacterToTalk) {
	int32 elapsedTime = _system->getMillis() - _oldTimer2;
	_oldTimer2 = _system->getMillis();
	_oldTimer = _oldTimer2;

	if (!_audioManager->voiceStillPlaying() && !waitCharacterToTalk) {
		_currentTextLine = 0;
		_currentTextLineId = -1;
	}

	updateCharacters(elapsedTime);
	updateAnimationSceneScripts(elapsedTime);
	updateTimer(elapsedTime);
	_animationManager->update(elapsedTime);
	_audioManager->updateAmbientSFX();
	render();
}

void ToonEngine::fixPaletteEntries(uint8 *palette, int num) {
	// some color values are coded on 6bits ( for old 6bits DAC )
	for (int32 i = 0; i < num * 3; ++i) {
		int32 a = palette[i];
		a = a * 4;
		if (a > 255)
			a = 255;
		palette[i] = a;
	}
}

// adapted from KyraEngine
void ToonEngine::updateAnimationSceneScripts(int32 timeElapsed) {
	static int32 numReentrant = 0;
	++numReentrant;
	const int startScript = _lastProcessedSceneScript;

	_updatingSceneScriptRunFlag = true;

	do {
		if (_sceneAnimationScripts[_lastProcessedSceneScript]._lastTimer <= _system->getMillis() &&
				!_sceneAnimationScripts[_lastProcessedSceneScript]._frozen && !_sceneAnimationScripts[_lastProcessedSceneScript]._frozenForConversation) {
			_animationSceneScriptRunFlag = true;

			while (_animationSceneScriptRunFlag && _sceneAnimationScripts[_lastProcessedSceneScript]._lastTimer <= _system->getMillis() && !_shouldQuit) {
				if (!_script->run(&_sceneAnimationScripts[_lastProcessedSceneScript]._state)) {
					_animationSceneScriptRunFlag = false;
				}

				//waitForScriptStep();

				if (_sceneAnimationScripts[_lastProcessedSceneScript]._frozen || _sceneAnimationScripts[_lastProcessedSceneScript]._frozenForConversation) {
					break;
				}
			}

		}

		if (!_script->isValid(&_sceneAnimationScripts[_lastProcessedSceneScript]._state)) {
			_script->start(&_sceneAnimationScripts[_lastProcessedSceneScript]._state, 9 + _lastProcessedSceneScript);
			_animationSceneScriptRunFlag = false;
		}

		++_lastProcessedSceneScript;
		if (_lastProcessedSceneScript >= state()->_locations[state()->_currentScene]._numSceneAnimations) {
			// cycle around
			_lastProcessedSceneScript = 0;
		}

	} while (_lastProcessedSceneScript != startScript && !_shouldQuit);

	_updatingSceneScriptRunFlag = false;
	--numReentrant;
}

void ToonEngine::loadScene(int32 SceneId, bool forGameLoad) {
	_firstFrame = true;

	_gameState->_lastVisitedScene = _gameState->_currentScene;
	_gameState->_currentScene = SceneId;

	_saveBufferStream->seek(0);

	if (SceneId == -1) {
		// this scene -1 is loaded at the very end of the game
		getAudioManager()->stopMusic();
		getMoviePlayer()->play("CREDITS.SMK");
		return;
	}

	// find out in what chapter we are (the script function ProcessToNextChapter is actually not called )
	// the location flag has the chapter info in it
	int32 flag = _gameState->_locations[SceneId]._flags;
	if (flag) {
		_gameState->_currentChapter = 0;
		do {
			_gameState->_currentChapter++;
			flag >>= 1;
		} while ((flag & 1) == 0);
	}

	for (int32 i = 0; i < 8; ++i) {
		if (_characters[i] != nullptr)
			_characters[i]->setFlag(0);
	}
	_drew->playStandingAnim();
	_drew->setVisible(true);

	// hide flux in chapter 2
	if (_gameState->_currentChapter == 1) {
		_flux->playStandingAnim();
		_flux->setVisible(true);
	} else {
		_flux->setVisible(false);
	}

	_lastMouseButton = 0;
	_mouseButton = 0;
	_currentHotspotItem = 0;

	if (!forGameLoad) {
		_gameState->_sackVisible = true;
		_gameState->_inCloseUp = false;
		_gameState->_inConversation = false;
		_gameState->_inInventory = false;
		_gameState->_inCutaway = false;
		_gameState->_currentScrollValue = 0;
		_gameState->_currentScrollLock = false;
		_gameState->_inCloseUp = false;
	}

	if (_gameState->_mouseState >= 0)
		addItemToInventory(_gameState->_mouseState);

	_gameState->_mouseState = -1;
	_mouseButton = 0;
	_lastMouseButton = 0x3;

	Common::String locationName = state()->_locations[SceneId]._name;

	// load package
	if (!resources()->openPackage(createRoomFilename(locationName + ".PAK"))) {
		const char *msg = _s("Unable to locate the '%s' data file.");
		Common::Path roomFileName = createRoomFilename(locationName + ".PAK");

		Common::U32String buf = Common::U32String::format(_(msg), roomFileName.toString(Common::Path::kNativeSeparator).c_str());
		GUIErrorMessage(buf);
		warning(msg, roomFileName.toString().c_str());
		_shouldQuit = true;
		return;
	}

	loadAdditionalPalette(Common::Path(locationName + ".NPP"), 0);

	_additionalPalette2Present = false;
	loadAdditionalPalette(Common::Path(locationName + ".NP2"), 1);

	loadAdditionalPalette(Common::Path(locationName + ".CUP"), 2);

	// load artwork
	delete _currentPicture;
	_currentPicture = new Picture(this);
	_currentPicture->loadPicture(Common::Path(locationName + ".CPS"));
	_currentPicture->setupPalette();

	delete _currentMask;
	_currentMask = new Picture(this);
	if (_currentMask->loadPicture(Common::Path(locationName + ".MSC")))
		_pathFinding->init(_currentMask);

	delete _roomTexts;
	_roomTexts = new TextResource(this);
	_roomTexts->loadTextResource(Common::Path(locationName + ".TRE"));

	uint32 fileSize;
	uint8 *sceneData = resources()->getFileData(Common::Path(locationName + ".DAT"), &fileSize);
	if (sceneData) {
		delete[] _roomScaleData;
		_roomScaleData = new uint8[fileSize];
		memcpy(_roomScaleData, sceneData, fileSize);
	}

	_audioManager->loadAudioPack(1, Common::Path(locationName + ".SVI"), createRoomFilename(locationName + ".SVL"));
	_audioManager->loadAudioPack(3, Common::Path(locationName + ".SEI"), Common::Path(locationName + ".SEL"));

	if (state()->_locations[SceneId]._flags & 0x40) {
		Common::String cutaway = state()->_locations[SceneId]._cutaway;
		_hotspots->loadRif(Common::Path(locationName + ".RIC"), Common::Path(cutaway + ".RIC"));
	} else {
		_hotspots->loadRif(Common::Path(locationName + ".RIC"), "");
	}
	restoreRifFlags(_gameState->_currentScene);

	uint32 convfileSize;
	uint8 *convData = resources()->getFileData(Common::Path(locationName + ".CNV"), &convfileSize);
	if (convData) {
		assert(convfileSize < 4096 * sizeof(int16));
		memcpy(_conversationData , convData, convfileSize);
		prepareConversations();
	}

	// load script

	_oldTimer = _system->getMillis();
	_oldTimer2 = _oldTimer;

	// fix the weird scaling issue during one frame when entering new scene
	_drew->update(0);
	_flux->update(0);

	_script->unload(&_scriptData);
	Common::String emcfile = locationName + ".EMC";
	_script->load(emcfile.c_str(), &_scriptData, &_script_func->_opcodes);
	_script->init(&_scriptState[0], &_scriptData);
	_script->init(&_scriptState[1], &_scriptData);
	_script->init(&_scriptState[2], &_scriptData);
	_script->init(&_scriptState[3], &_scriptData);

	//_script->RoomScript->Decompile("decomp.txt");
	//RoomScript->Decompile2("decomp2.txt");

	for (int i = 0; i < state()->_locations[SceneId]._numSceneAnimations; ++i) {
		_sceneAnimationScripts[i]._data = &_scriptData;
		_script->init(&_sceneAnimationScripts[i]._state, _sceneAnimationScripts[i]._data);
		if (!forGameLoad) {
			_script->start(&_sceneAnimationScripts[i]._state, 9 + i);
			_sceneAnimationScripts[i]._lastTimer = _system->getMillis();
			_sceneAnimationScripts[i]._frozen = false;
			_sceneAnimationScripts[i]._frozenForConversation = false;
		}
	}

	playRoomMusic();

	_lastProcessedSceneScript = 0;
	_gameState->_locations[SceneId]._visited = true;

	setupGeneralPalette();
	createShadowLUT();

	state()->_mouseHidden = false;

	clearDirtyRects();
	dirtyAllScreen();

	if (!forGameLoad) {

		_script->start(&_scriptState[0], 0);

		while (_script->run(&_scriptState[0]))
			waitForScriptStep();

		_script->start(&_scriptState[0], 8);

		while (_script->run(&_scriptState[0]))
			waitForScriptStep();

		if (_gameState->_nextSpecialEnterX != -1 && _gameState->_nextSpecialEnterY != -1) {
			_drew->forcePosition(_gameState->_nextSpecialEnterX, _gameState->_nextSpecialEnterY);
			_gameState->_nextSpecialEnterX = -1;
			_gameState->_nextSpecialEnterY = -1;
		}

		_script->start(&_scriptState[0], 3);

		while (_script->run(&_scriptState[0]))
			waitForScriptStep();

		_script->start(&_scriptState[0], 4);

		while (_script->run(&_scriptState[0]))
			waitForScriptStep();

	}
}

void ToonEngine::setupGeneralPalette() {
	setPaletteEntries(_additionalPalette1, 232, 23);
	setPaletteEntries(_universalPalette, 200, 32);
	setPaletteEntries(_fluxPalette, 192, 8);

	if (_drew)
		_drew->setupPalette();
}

void ToonEngine::loadAdditionalPalette(const Common::Path &fileName, int32 mode) {

	uint32 size = 0;
	uint8 *palette = resources()->getFileData(fileName, &size);
	if (!palette)
		return;

	switch (mode) {
	case 0:
		memcpy(_additionalPalette1, palette, 69);
		fixPaletteEntries(_additionalPalette1, 23);
		break;
	case 1:
		memcpy(_additionalPalette2, palette, 69);
		fixPaletteEntries(_additionalPalette2, 23);
		_additionalPalette2Present = true;
		break;
	case 2:
		memcpy(_cutawayPalette, palette, size);
		fixPaletteEntries(_cutawayPalette, size/3);
		break;
	case 3:
		memcpy(_universalPalette, palette, 96);
		fixPaletteEntries(_universalPalette, 32);
		break;
	case 4:
		memcpy(_fluxPalette, palette, 24);
		fixPaletteEntries(_fluxPalette , 8);
		break;
	default:
		warning("loadAdditionalPalette() - Unknown mode");
	}
}

void ToonEngine::initChapter() {

	EMCData data;
	EMCState status;
	memset(&data, 0, sizeof(data));
	memset(&status, 0, sizeof(status));

	delete _script;
	_script = new EMCInterpreter(this);

	_script->load("_START01.EMC", &data, &_script_func->_opcodes);
	_script->init(&status, &data);
	_script->start(&status, 0);
	while (_script->run(&status))
		waitForScriptStep();

	_script->unload(&data);

	setupGeneralPalette();
}

void ToonEngine::loadCursor() {
	delete _cursorAnimation;
	_cursorAnimation = new Animation(this);
	_cursorAnimation->loadAnimation("MOUSE.CAF");
	delete _cursorAnimationInstance;
	_cursorAnimationInstance = _animationManager->createNewInstance(kAnimationCursor);
	_cursorAnimationInstance->setAnimation(_cursorAnimation);
	_cursorAnimationInstance->setVisible(true);
	_cursorAnimationInstance->setFrame(0);
	_cursorAnimationInstance->setAnimationRange(0, 0);
	_cursorAnimationInstance->setFps(8);

	setCursor(5);
}

void ToonEngine::setCursor(int32 type, bool inventory, int32 offsetX, int offsetY) {

	static const int32 offsets[] = {
		0,   1,  1,  6,  7,  1,  8,   10, 18,  10,
		28,  8,  36, 10, 46, 10, 56,  10, 66,  10,
		76,  10, 86, 10, 96, 10, 106, 10, 116, 10,
		126, 10
	};

	if (!inventory) {
		_cursorAnimationInstance->setAnimation(_cursorAnimation);
		_cursorAnimationInstance->setAnimationRange(offsets[type * 2 + 0], offsets[type * 2 + 0] + offsets[type * 2 + 1] - 1);
		_cursorAnimationInstance->playAnimation();
	} else {
		_cursorAnimationInstance->setAnimation(_inventoryIcons);
		_cursorAnimationInstance->setAnimationRange(type, type);
		_cursorAnimationInstance->playAnimation();
	}

	_cursorOffsetX = offsetX;
	_cursorOffsetY = offsetY;
}

void ToonEngine::setSceneAnimationScriptUpdate(bool enable) {
	_animationSceneScriptRunFlag = enable;
}

bool ToonEngine::isUpdatingSceneAnimation() {
	return _updatingSceneScriptRunFlag;
}

int32 ToonEngine::getCurrentUpdatingSceneAnimation() {
	return _lastProcessedSceneScript;
}

int32 ToonEngine::randRange(int32 minStart, int32 maxStart) {
	return _rnd.getRandomNumberRng(minStart, maxStart);
}

int32 ToonEngine::runEventScript(int32 x, int32 y, int32 mode, int32 id, int32 scriptId) {

	if (_currentScriptRegion >= 4)
		return 0;

	EMCState *status = &_scriptState[_currentScriptRegion];
	_script->init(status, &_scriptData);

	// setup registers
	status->regs[0] = x;
	status->regs[1] = y;
	status->regs[2] = 0;
	status->regs[3] = 0;
	status->regs[4] = _gameState->_mouseState; //
	status->regs[5] = 0;
	status->regs[6] = scriptId;
	status->regs[7] = mode;
	status->regs[8] = id;

	++_currentScriptRegion;

	_script->start(status, 1);
	while (_script->run(status) && !_shouldQuit) {
		waitForScriptStep();
	}
	--_currentScriptRegion;

	return status->regs[2];
}

void ToonEngine::clickEvent() {
	bool leftButton = false;
	bool rightButton = false;

	if ((_lastMouseButton & 0x1) == 0 && (_mouseButton & 0x1) == 1)
		leftButton = true;
	if ((_lastMouseButton & 0x2) == 0 && (_mouseButton & 0x2) == 2)
		rightButton = true;

	_lastMouseButton = _mouseButton;
	if (!leftButton && !rightButton)
		return;

	if (_gameState->_sackVisible) {
		if (_mouseX > 0 && _mouseX < 40 && _mouseY > 356 && _mouseY < 396) {
			if (_gameState->_mouseState >= 0 && !rightButton) {
				addItemToInventory(_gameState->_mouseState);
				setCursor(0, false, 0, 0);
				_currentHotspotItem = 0;
				return;
			} else {
				showInventory();
			}
			return;
		}
	}

	// with inventory
	if (rightButton && _gameState->_mouseState >= 0) {
		addItemToInventory(_gameState->_mouseState);
		setCursor(0, false, 0, 0);
		_currentHotspotItem = 0;
		return;
	}

	int32 mouseX = _mouseX;
	if (_gameState->_inCutaway) {
		mouseX += TOON_BACKBUFFER_WIDTH;
	}

	// find hotspot
	int32 hot = _hotspots->find(mouseX + state()->_currentScrollValue , _mouseY);
	HotspotData *currentHot = 0;
	if (hot > -1) {
		currentHot = _hotspots->get(hot);
	}

	if (_currentHotspotItem == -3) {
		if (_gameState->_mouseState <= 0) {
			if (leftButton)
				createMouseItem(104);
			else
				characterTalk(1104);
		}
		// Don't walk to Flux after clicking on him
		return;
	}
	if (_currentHotspotItem == -4) {
		if (_gameState->_mouseState >= 0) {
			if (leftButton)
				if (!handleInventoryOnInventory(0, _gameState->_mouseState)) {
					playSoundWrong();
				}
			return;
		}
	}

	if (!currentHot) {
		int16 xx, yy;

		if (_gameState->_inCutaway || _gameState->_inInventory || _gameState->_inCloseUp)
			return;

		if (_pathFinding->findClosestWalkingPoint(_mouseX + _gameState->_currentScrollValue , _mouseY, &xx, &yy))
			_drew->walkTo(xx, yy);

		return;
	}

	int commandId = 0;
	if (_gameState->_mouseState < 0) {
		// left or right click
		if (rightButton)
			commandId = 2 + 8;
		else
			commandId = 0 + 8;
	} else {
		commandId = 2 * (_gameState->_mouseState - 1) + 16;
	}

	_drew->stopWalk();

	int16 command = currentHot->getData(commandId);
	int16 argument = currentHot->getData(commandId + 1);
	int16 priority = currentHot->getPriority();

	if (!_gameState->_inCutaway && !_gameState->_inCloseUp) {
		if (leftButton && (currentHot->getData(4) != 2 || _gameState->_mouseState >= 0) && currentHot->getData(5) != -1) {
			if (currentHot->getData(5)) {
				if (!_drew->walkTo(currentHot->getData(5), currentHot->getData(6))) {
					// walk was canceled ?
					return;
				}
			} else {
				if (!_drew->walkTo(_mouseX + _gameState->_currentScrollValue, _mouseY)) {
					// walk was canceled ?
					return;
				}
			}
		}
	}

	int32 result = 0;

	switch (command) {
	case 1:
		sayLines(1, argument);
		break;
	case 2:
		result = runEventScript(_mouseX, _mouseY, command, argument, priority);
		break;
	case 3:
		runEventScript(_mouseX, _mouseY, command, argument, priority);
		result = 0;
		break;
	case 4:
		playSFX(argument, 128);
		break;
	case 5:
		break;
	case 6:
		createMouseItem(argument);
		currentHot->setData(7, -1);
		break;
	case 7:
		// switch to CloseUp
		break;
	case 8:
		// face flux
		sayLines(1, argument);
		break;
	case 9:
	case 10:
		sayLines(2, argument);
		break;
	case 11:
		sayLines(3, argument);
		break;
	default:
		playSoundWrong();
		return;
	}

	if (result == 3) {
		int32 val = _scriptState[_currentScriptRegion].regs[4];
		currentHot->setData(4, currentHot->getData(4) & val);
	}
	if (result == 2 || result == 3) {
		int32 val = _scriptState[_currentScriptRegion].regs[6];
		currentHot->setData(7, val);
	}

	if (result == 1) {
		int32 val = _scriptState[_currentScriptRegion].regs[4];
		currentHot->setData(4, currentHot->getData(4) & val);
	}
}

void ToonEngine::selectHotspot() {
	int16 x1 = 0;
	int16 x2 = 0;
	int16 y1 = 0;
	int16 y2 = 0;

	int16 mouseX = _mouseX;

	if (_gameState->_inCutaway)
		mouseX += TOON_BACKBUFFER_WIDTH;

	if (_gameState->_sackVisible) {
		if (_mouseX > 0 && _mouseX < 40 && _mouseY > 356 && _mouseY < 396) {
			_currentHotspotItem = -2;

			if (_gameState->_mouseState < 0) {
				int mode = 3;
				setCursor(mode);
			} else {
				setCursor(_gameState->_mouseState, true, -18, -14);
			}

			return;
		}
	}

	if (_gameState->_mouseState > 0) {
		// picked drew?
		getDrew()->getAnimationInstance()->getRect(&x1, &y1, &x2, &y2);
		if (_mouseX + _gameState->_currentScrollValue >= x1 && _mouseX + _gameState->_currentScrollValue <= x2 && _mouseY >= y1 && _mouseY <= y2) {
			_currentHotspotItem = -4;
			return;
		}
	}

	if (getFlux()->getVisible()) {
		getFlux()->getAnimationInstance()->getRect(&x1, &y1, &x2, &y2);
		if (_mouseX + _gameState->_currentScrollValue >= x1 && _mouseX + _gameState->_currentScrollValue <= x2 && _mouseY >= y1 && _mouseY <= y2) {
			_currentHotspotItem = -3;

			if (_gameState->_mouseState < 0) {
				int mode = 3;
				setCursor(mode);
			} else {
				setCursor(_gameState->_mouseState, true, -18, -14);
			}

			return;
		}
	}

	int32 hot = _hotspots->find(mouseX + state()->_currentScrollValue, _mouseY);
	if (hot != -1) {
		HotspotData *hotspot = _hotspots->get(hot);
		int32 item = hotspot->getData(14);
		if (hotspot->getType() == 3)
			item += 2000;

		// update palette based on ticks if we're in "use from inventory mode"
		if (_gameState->_mouseState >= 0) {

			int32 tick = _system->getMillis() / _tickLength;
			int32 animReverse = tick & 0x10;
			int32 animStep = tick & 0xf;

			byte color[3];
			if (animReverse == 0) {
				color[0] = 16 * animStep;
				color[1] = 0;
				color[2] = 0;
			} else {
				color[0] = 16 * (15 - animStep);
				color[1] = 0;
				color[2] = 0;
			}
			setPaletteEntries(color, 255, 1);
		}

#if 0
		if (item == _currentHotspotItem)
			return;
#endif
		_currentHotspotItem = item;
		if (_gameState->_mouseState < 0) {
			int mode = hotspot->getMode();
			setCursor(mode);
		} else {
			setCursor(_gameState->_mouseState, true, -18, -14);
		}
	} else {
		_currentHotspotItem = 0;

		if (_gameState->_mouseState < 0) {
			setCursor(0);
		} else {
			byte color[3];
			color[0] = 0;
			color[1] = 0;
			color[2] = 0;
			setCursor(_gameState->_mouseState, true, -18, -14);
			setPaletteEntries(color, 255, 1);
		}
	}
}

void ToonEngine::exitScene() {
	fadeOut(5);

	// disable all scene animation
	for (int32 i = 0; i < 64; ++i) {
		if (_sceneAnimations[i]._active) {
			delete _sceneAnimations[i]._animation;
			_sceneAnimations[i]._active = false;
			_animationManager->removeInstance(_sceneAnimations[i]._animInstance);

			// see if one character shares this instance
			for (int32 c = 0; c < 32; ++c) {
				if (_characters[c] != nullptr && _characters[c]->getAnimationInstance() == _sceneAnimations[i]._animInstance) {
					_characters[c]->setAnimationInstance(nullptr);
				}
			}

			delete _sceneAnimations[i]._originalAnimInstance;
			_sceneAnimations[i]._animInstance = nullptr;
			_sceneAnimations[i]._animation = nullptr;
			_sceneAnimations[i]._originalAnimInstance = nullptr;
		}
	}
	for (int32 i = 0; i < 64; ++i) {
		_sceneAnimationScripts[i]._frozen = true;
		_sceneAnimationScripts[i]._active = false;
	}

	// remove all characters except drew and flux
	for (int32 i = 0; i < 8; ++i) {
		if (_characters[i] != _drew && _characters[i] != _flux) {
			if (_characters[i] != nullptr) {
				delete _characters[i];
				_characters[i] = nullptr;
			}
		} else {
			_characters[i]->stopSpecialAnim();
		}
	}

	for (int32 i = 0; i < 2; ++i) {
		_gameState->_timerEnabled[i] = false;
	}

	// put back our item in inventory if needed
	if (_gameState->_mouseState >= 0) {
		addItemToInventory(_gameState->_mouseState);
		_gameState->_mouseState = -1;
	}

	_audioManager->killAllAmbientSFX();
	_audioManager->stopAllSfxs();
	_audioManager->stopCurrentVoice();
	_currentTextLine = 0;
	_currentTextLineId = -1;
	_currentTextLineCharacterId = 0;

	Common::String locationName = _gameState->_locations[_gameState->_currentScene]._name;
	resources()->closePackage(createRoomFilename(locationName + ".PAK"));

	_drew->stopWalk();
	_flux->stopWalk();

	storeRifFlags(_gameState->_currentScene);
}

// flip between the cutaway scene and the normal scene
void ToonEngine::flipScreens() {
	_gameState->_inCloseUp = !_gameState->_inCloseUp;

	if (_gameState->_inCloseUp) {
		_gameState->_currentScrollValue = TOON_SCREEN_WIDTH;
		setPaletteEntries(_cutawayPalette, 1, 128);
		if (_additionalPalette2Present)
			setPaletteEntries(_additionalPalette2, 232, 23);
	} else {
		_gameState->_currentScrollValue = 0;
		_currentPicture->setupPalette();
		setupGeneralPalette();
	}
	flushPalette();
}

void ToonEngine::fadeIn(int32 numFrames) {
	for (int32 f = 0; f < numFrames; ++f) {

		uint8 vmpalette[3 * 256];
		for (int32 i = 0; i < 256; ++i) {
			vmpalette[i * 3 + 0] = f * _finalPalette[i * 3 + 0] / (numFrames - 1);
			vmpalette[i * 3 + 1] = f * _finalPalette[i * 3 + 1] / (numFrames - 1);
			vmpalette[i * 3 + 2] = f * _finalPalette[i * 3 + 2] / (numFrames - 1);
		}
		_system->getPaletteManager()->setPalette(vmpalette, 0, 256);
		_system->updateScreen();
		_system->delayMillis(_tickLength);
	}
}

void ToonEngine::fadeOut(int32 numFrames) {

	uint8 oldpalette[3 * 256];
	_system->getPaletteManager()->grabPalette(oldpalette, 0, 256);

	for (int32 f = 0; f < numFrames; ++f) {
		uint8 vmpalette[3 * 256];
		for (int32 i = 0; i < 256; ++i) {
			vmpalette[i * 3 + 0] = (numFrames - f - 1) * oldpalette[i * 3 + 0] / (numFrames - 1);
			vmpalette[i * 3 + 1] = (numFrames - f - 1) * oldpalette[i * 3 + 1] / (numFrames - 1);
			vmpalette[i * 3 + 2] = (numFrames - f - 1) * oldpalette[i * 3 + 2] / (numFrames - 1);
		}
		_system->getPaletteManager()->setPalette(vmpalette, 0, 256);
		_system->updateScreen();
		_system->delayMillis(_tickLength);
	}
}

void ToonEngine::initFonts() {
	_fontRenderer = new FontRenderer(this);
	if (_isEnglishDemo) {
		// The English demo uses a different font format. There is only one
		// font, so the alternative font setting is ignored.
		_fontRenderer->loadDemoFont("8FAT.FNT");
	} else {
		_fontToon = new Animation(this);
		_fontToon->loadAnimation("TOONFONT.CAF");

		_fontEZ = new Animation(this);
		_fontEZ->loadAnimation("EZFONT.CAF");
	}

	setFont(false);
}

void ToonEngine::setFont(bool alternative) {
	if (alternative) {
		_currentFont = _fontEZ;
	} else {
		_currentFont = _fontToon;
	}
	_useAlternativeFont = alternative;
}

void ToonEngine::drawInfoLine() {
	if (_currentHotspotItem != 0 && !_gameState->_mouseHidden && !_gameState->_inConversation) {
		const char *infoTool = nullptr;
		if (_currentHotspotItem >= 0 && _currentHotspotItem < 2000) {
			infoTool = _roomTexts->getText(_currentHotspotItem);
		} else if (_currentHotspotItem <= -1) {
//			static const char * const specialInfoLine[] = { "Exit non defined", "Bottomless Bag", "Flux", "Drew Blanc" };
			infoTool = _specialInfoLine[-1 - _currentHotspotItem];
		} else {
			int32 loc = _currentHotspotItem - 2000;
			// location names are hardcoded ...
			infoTool = getLocationString(loc, _gameState->_locations[loc]._visited);
		}
		if (infoTool) {
			_fontRenderer->setFontColor(0xc8, 0xdd, 0xe3);
			_fontRenderer->setFont(_currentFont);
			_fontRenderer->renderText(320 + _gameState->_currentScrollValue, 398, infoTool, 5);
		}
	}
}

Common::WriteStream *ToonEngine::getSaveBufferStream() {
	return _saveBufferStream;
}

const char *ToonEngine::getLocationString(int32 locationId, bool alreadyVisited) {
	if (alreadyVisited)
		return _locationDirVisited[locationId];
	else
		return _locationDirNotVisited[locationId];
}

int32 ToonEngine::getScaleAtPoint(int32 x, int32 y) {
	if (!_currentMask)
		return 1024;

	// clamp values
	x = MIN<int32>(TOON_BACKBUFFER_WIDTH - 1, MAX<int32>(0, x));
	y = MIN<int32>(TOON_BACKBUFFER_HEIGHT - 1, MAX<int32>(0, y));

	int32 maskData = _currentMask->getData(x, y) & 0x1f;
	return _roomScaleData[maskData + 2] * 1024 / 100;
}

int32 ToonEngine::getLayerAtPoint(int32 x, int32 y) {
	if (!_currentMask)
		return 0;

	// clamp values
	x = MIN<int32>(TOON_BACKBUFFER_WIDTH - 1, MAX<int32>(0, x));
	y = MIN<int32>(TOON_BACKBUFFER_HEIGHT - 1, MAX<int32>(0, y));

	int32 maskData = _currentMask->getData(x, y) & 0x1f;
	return _roomScaleData[maskData + 130] << 5;
}

int32 ToonEngine::getZAtPoint(int32 x, int32 y) {
	if (!_currentMask)
		return 0;
	return _currentMask->getData(x, y) & 0x1f;
}

void ToonEngine::storeRifFlags(int32 location) {

	if (_gameState->_locations[location]._numRifBoxes != _hotspots->getCount()) {
		_gameState->_locations[location]._numRifBoxes = _hotspots->getCount();
	}

	for (int32 i = 0; i < _hotspots->getCount(); ++i) {
		_gameState->_locations[location]._rifBoxesFlags[i * 2 + 0] = _hotspots->get(i)->getData(4);
		_gameState->_locations[location]._rifBoxesFlags[i * 2 + 1] = _hotspots->get(i)->getData(7);
	}
}

void ToonEngine::restoreRifFlags(int32 location) {
	if (_hotspots) {
		if (!_gameState->_locations[location]._visited) {
			for (int32 i = 0; i < _hotspots->getCount(); ++i) {
				_gameState->_locations[location]._rifBoxesFlags[i * 2 + 0] = _hotspots->get(i)->getData(4);
				_gameState->_locations[location]._rifBoxesFlags[i * 2 + 1] = _hotspots->get(i)->getData(7);
			}
			_gameState->_locations[location]._numRifBoxes = _hotspots->getCount();
		} else {
			if (_gameState->_locations[location]._numRifBoxes != _hotspots->getCount())
				return;

			for (int32 i = 0; i < _hotspots->getCount(); ++i) {
				_hotspots->get(i)->setData(4, _gameState->_locations[location]._rifBoxesFlags[i * 2 + 0]);
				_hotspots->get(i)->setData(7, _gameState->_locations[location]._rifBoxesFlags[i * 2 + 1]);
			}
		}
	}
}

void ToonEngine::sayLines(int numLines, int dialogId) {
	// exit conversation state

	// if (inInventory)
	// 	character_talks(dialogid, -1, 0, 0);
	// else

#if 0
	int oldShowMouse = 0;

	if (Game.MouseHiddenCount <= 0) {
		Game.MouseHiddenCount = 1;
		oldShowMouse = 1;
	}
#endif

	int32 currentLine = dialogId;

	for (int32 i = 0; i < numLines; ++i) {
		if (!characterTalk(currentLine))
			break;

		while (_audioManager->voiceStillPlaying() && !_shouldQuit)
			doFrame();

		// find next line
		if (currentLine < 1000)
			currentLine = _roomTexts->getNext(currentLine);
		else
			currentLine = _genericTexts->getNext(currentLine - 1000) + 1000;
	}

#if 0
	if (oldShowMouse)
		Game.MouseHiddenCount = 0;
#endif
}

int32 ToonEngine::simpleCharacterTalk(int32 dialogid) {
	int32 myId = 0;

	if (_audioManager->voiceStillPlaying())
		_audioManager->stopCurrentVoice();

	if (dialogid < 1000) {
		myId = _roomTexts->getId(dialogid);
		_audioManager->playVoice(myId, false);
	} else {
		myId = _genericTexts->getId(dialogid - 1000);
		if (myId == -1)
			return 0;
		_audioManager->playVoice(myId, true);
	}

	return 1;
}

void ToonEngine::playTalkAnimOnCharacter(int32 animID, int32 characterId, bool talker) {
	if (animID || talker) {
		if (characterId == 0) {
			_drew->playAnim(animID, 0, (talker ? 8 : 0) + 2);
		} else if (characterId == 1) {
			// stop flux if he is walking
			if (_flux->getFlag() & 1) {
				_flux->stopWalk();
			}
			_flux->playAnim(animID, 0, (talker ? 8 : 0) + 2);
			_flux->setFlag(_flux->getFlag() | 1);
		} else {
			Character *character = getCharacterById(characterId);
			if (character) {
				character->playAnim(animID, 0, (talker ? 8 : 0) + 2);
			}
		}
	} else {
		Character *character = getCharacterById(characterId);
		if (character)
			character->setAnimFlag(character->getAnimFlag() | 1);
	}
}

int32 ToonEngine::characterTalk(int32 dialogid, bool blocking) {
	if (blocking == false && _audioManager->voiceStillPlaying()) {
		if (_currentTextLineCharacterId == 0 || _currentTextLineCharacterId == 1) {
			// Drew or Flux is already talking, and this voice is not important
			// skip it
			return 0;
		}
	}

	char *myLine;
	if (dialogid < 1000)
		myLine = _roomTexts->getText(dialogid);
	else
		myLine = _genericTexts->getText(dialogid - 1000);

	if (!myLine)
		return 0;

	bool oldMouseHidden = _gameState->_mouseHidden;
	if (blocking)
		_gameState->_mouseHidden = true;


	// get what is before the string
	int a = READ_LE_UINT16(myLine - 2);
	char *b = myLine - 2 - 4 * a;

	char *c = b - 2; // v6
	int numParticipants = READ_LE_UINT16(c); // num dialogue participants

	char *e = c - 2 - 4 * numParticipants;
	READ_LE_UINT16(e);

	// if one voice is still playing, wait !
	if (blocking) {
		while (_audioManager->voiceStillPlaying() && !_shouldQuit)
			doFrame();

		char *cc = c;
		Character *waitChar;
		for (int32 i = 0; i < numParticipants - 1; ++i) {
			// listener
			int32 listenerId = READ_LE_UINT16(cc - 2);
			cc -= 4;
			waitChar = getCharacterById(listenerId);
			if (waitChar) {
				while ((waitChar->getAnimFlag() & 0x10) == 0x10 && !_shouldQuit)
					doFrame();
			}

		}
		int32 talkerId = READ_LE_UINT16(cc - 2);

		waitChar = getCharacterById(talkerId);
		if (waitChar && !_gameState->_inInventory) {
			while ((waitChar->getAnimFlag() & 0x10) == 0x10 && !_shouldQuit)
				doFrame();
		}
	} else if (_audioManager->voiceStillPlaying())
		_audioManager->stopCurrentVoice();

	for (int32 i = 0; i < numParticipants - 1; ++i) {
		// listener
		int32 listenerId = READ_LE_UINT16(c - 2);
		int32 listenerAnimId = READ_LE_UINT16(c - 4);
		if (blocking) playTalkAnimOnCharacter(listenerAnimId, listenerId, false);
		c -= 4;
	}

	int32 talkerId = READ_LE_UINT16(c - 2);
	int32 talkerAnimId = READ_LE_UINT16(c - 4);

	_currentTextLine = myLine;
	_currentTextLineCharacterId = talkerId;
	_currentTextLineId = dialogid;

	if (blocking) {
		Character *character = getCharacterById(talkerId);
		if (character)
			character->setTalking(true);

		playTalkAnimOnCharacter(talkerAnimId, talkerId, true);

		// set once more the values, they may have been overwritten when the engine
		// waits for the character to be ready.
		_currentTextLine = myLine;
		_currentTextLineCharacterId = talkerId;
		_currentTextLineId = dialogid;
	} else {
		Character *character = getCharacterById(talkerId);
		if (character)
			character->stopSpecialAnim();
	}

	debugC(0, 0xfff, "Talker = %d (num participants : %d) will say '%s'", (int)talkerId , (int)numParticipants, myLine);

	getTextPosition(talkerId, &_currentTextLineX, &_currentTextLineY);

	if (dialogid < 1000) {
		int myId = _roomTexts->getId(dialogid);
		_audioManager->playVoice(myId, false);
	} else {
		int myId = _genericTexts->getId(dialogid - 1000);
		_audioManager->playVoice(myId, true);
	}

	if (blocking) {
		while (_audioManager->voiceStillPlaying() && !_shouldQuit)
			doFrame();
		_gameState->_mouseHidden = oldMouseHidden && _gameState->_mouseHidden;

		Character *character = getCharacterById(talkerId);
		if (character)
			character->setTalking(false);
	}
	return 1;
}

void ToonEngine::haveAConversation(int32 convId) {
	setCursor(0);

	_gameState->_inConversation = true;
	_gameState->_showConversationIcons = false;
	_gameState->_exitConversation = false;
	_gameState->_sackVisible = false;
	Conversation *conv = &state()->_conversationState[convId];
	_gameState->_currentConversationId = convId;

	// change the music to the "conversation" music if needed.
	playRoomMusic();

	if (conv->_enable) {
		// fix dialog script based on new flags
		for (int32 i = 0; i < 10; ++i) {
			if (conv->state[i]._data2 == 1 || conv->state[i]._data2 == 3) {
				if (getConversationFlag(_gameState->_currentScene, conv->state[i]._data3))
					conv->state[i]._data2 = 1;
				else
					conv->state[i]._data2 = 3;
			}
		}

		// if current voice stream sub 15130
		processConversationClick(conv , 2);
		doFrame();
	}

	_mouseButton = 0;
	_gameState->_firstConverstationLine = true;

	while (!_gameState->_exitConversation && !_shouldQuit) {
		_gameState->_mouseHidden = false;
		_gameState->_showConversationIcons = true;
		int32 oldMouseButton = _mouseButton;
		while (!_shouldQuit) {
			doFrame();

			if (_mouseButton != 0) {
				if (!oldMouseButton)
					break;
			} else {
				oldMouseButton = 0;
			}
		}
		int selected = -1;
		int a = 0;
		for (int i = 0; i < 10; ++i) {
			if (conv->state[i]._data2 == 1) {
				if (_mouseX > 50 + a * 60 && _mouseX < 100 + a * 60 && _mouseY >= 336 && _mouseY <= 386) {
					selected = i;
					break;
				}
				++a;
			}
		}

		if (_shouldQuit)
			return;

		_gameState->_showConversationIcons = false;
		_gameState->_mouseHidden = true;

		if (selected < 0 || selected == 1 || selected == 3) {
			if (_gameState->_firstConverstationLine)
				processConversationClick(conv, 3);
			else
				processConversationClick(conv, 1);
			break;
		} else {
			processConversationClick(conv, selected);
		}
	}

	for (int i = 0; i < 10; ++i) {
		if (conv->state[i]._data2 == 2) {
			if (i != 3)
				conv->state[i]._data2 = 1;
		}
	}

	_gameState->_exitConversation = false;
	_gameState->_inConversation = false;
	_gameState->_currentConversationId = -1;
	_gameState->_mouseHidden = false;
	_gameState->_sackVisible = true;

	// switch back to original music
	playRoomMusic();
}

void ToonEngine::drawConversationIcons() {
	if (!_gameState->_inConversation || !_gameState->_showConversationIcons)
		return;
	int32 aa = 50 + _gameState->_currentScrollValue;
	for (int32 i = 0; i < 10; ++i) {
		if (_gameState->_conversationState[_gameState->_currentConversationId].state[i]._data2 == 1) {
			_dialogIcons->drawFrame(*_mainSurface, (i + _gameState->_currentScene) & 7, aa, 336);
			_dialogIcons->drawFrame(*_mainSurface, 7 + _gameState->_conversationState[_gameState->_currentConversationId].state[i]._data3, aa, 339);
			aa += 60;
		}
	}
}

void ToonEngine::prepareConversations() {
	Conversation *allConvs = _gameState->_conversationState;
	for (int32 i = 0; i < 60; ++i) {

		allConvs[i].state[0]._data2 = 1;
		if (!allConvs[i].state[0]._data3) {
			allConvs[i].state[0]._data3 = 1;
		}
		allConvs[i].state[1]._data2 = 1;
		allConvs[i].state[1]._data3 = 6;
		allConvs[i].state[3]._data2 = 2;

	}
	int numConversations = READ_LE_UINT16(_conversationData + 1);
	int16 *curConversation = _conversationData + 3;
	for (int i = 0; i < numConversations; ++i) {
		Conversation *conv = &allConvs[ READ_LE_UINT16(curConversation)];
		if (!conv->_enable) {

			conv->_enable = 1;

			int16 offset1 = READ_LE_UINT16(curConversation + 1);
			void *convData1 = (char *)_conversationData + offset1;
			conv->state[0]._data4 = convData1;

			int16 offset2 = READ_LE_UINT16(curConversation + 2);
			void *convData2 = (char *)_conversationData + offset2;
			conv->state[1]._data4 = convData2;

			int16 offset3 = READ_LE_UINT16(curConversation + 3);
			void *convData3 = (char *)_conversationData + offset3;
			conv->state[2]._data4 = convData3;

			int16 offset4 = READ_LE_UINT16(curConversation + 4);
			void *convData4 = (char *)_conversationData + offset4;
			conv->state[3]._data4 = convData4;
		}
		curConversation += 5;
	}
}

void ToonEngine::processConversationClick(Conversation *conv, int32 status) {
	Conversation::ConvState *v2 = (Conversation::ConvState *)&conv->state[status];

	int16 *i = (int16 *)((char *)v2->_data4 + 2);

	_gameState->_firstConverstationLine = false;
	while (READ_LE_INT16(i) >= 0) {
		if (READ_LE_INT16(i) < 100) {
			if (_gameState->_exitConversation == false) {
				characterTalk(READ_LE_INT16(i + 1));
			}
		} else {
			runConversationCommand(&i);
		}
		i += 2;
	}

	int16 command = READ_LE_INT16(i);
	int16 value = READ_LE_INT16(i + 1);

	if (command == -1) {
		v2->_data2 = 0;
	} else if (command == -2) {
		v2->_data4 = (char *)_conversationData + value;
		v2->_data3 = READ_LE_INT16(v2->_data4);
	} else if (command == -3) {
		v2->_data2 = 2;
		v2->_data4 = (char *)_conversationData + value;
		v2->_data3 = READ_LE_INT16(v2->_data4);
	}

	int16 *v7 = i + 2;
	int16 v8 = READ_LE_INT16(v7);
	if (v8 == -1) {
		_gameState->_mouseHidden = false;
	} else {
		while (v8 != -1) {
			++v7;
			int16 *v14 = (int16 *)((char *)_conversationData + v8);

			// find free dialogue slot
			for (int j = 0; j < 10; ++j) {
				if (!conv->state[j]._data2) {
					conv->state[j]._data3 = READ_LE_INT16(v14);
					conv->state[j]._data4 = v14;
					if (getConversationFlag(_gameState->_currentScene, conv->state[j]._data3))
						conv->state[j]._data2 = 1;
					else
						conv->state[j]._data2 = 3;

					v8 = READ_LE_INT16(v7);
					if (v8 == -1)
						return;
					else
						break; // restarts while loop;
				}
			}
		}
	}
}

// hardcoded conversation flag to know if one dialogue icon must be displayed or not
// based on game events...
int32 ToonEngine::getConversationFlag(int32 locationId, int32 param) {
	if (locationId == 1) {
		if (param == 0x34)
			return _gameState->getGameFlag(93);

		if (param != 55)
			return 1;

		if (!_gameState->getGameFlag(262))
			return 1;

		return 0;
	} else if (locationId == 2) {
		if (param == 36 && _gameState->getGameFlag(149))
			return 0;
		return 1;
	} else if (locationId == 7) {
		if (param == 30)
			return _gameState->getGameFlag(132);
		else
			return 1;
	} else if (locationId == 8) {
		if (param == 0x20) {
			if (!_gameState->getGameFlag(73) && !_gameState->getGameFlag(151) && !_gameState->getGameFlag(152) && !_gameState->getGameFlag(153))
				return 1;
			return 0;
		}
		if (param == 33) {
			if (!_gameState->getGameFlag(73) && !_gameState->getGameFlag(151) && !_gameState->getGameFlag(152) && !_gameState->getGameFlag(153))
				return 0;
			return 1;
		}
	} else if (locationId == 0xb) {
		if (param == 0x12) {
			if (!_gameState->hasItemInInventory(71))
				return 1;
			else
				return 0;
		}
		if (param == 74) {
			if (_gameState->hasItemInInventory(71))
				return 1;
			else
				return 0;
		}
		return 1;
	} else if (locationId == 0xc) {
		if (param == 0x3d && _gameState->getGameFlag(154)) {
			return 0;
		}
		if (param == 76 && !_gameState->getGameFlag(79)) {
			return 0;
		}
		if (param == 0x4e && !_gameState->hasItemInInventory(32)) {
			return 0;
		}
		if (param == 0x4f && !_gameState->hasItemInInventory(92)) {
			return 0;
		}
		if (param == 80 && !_gameState->hasItemInInventory(91)) {
			return 0;
		}
		if (param == 0x4d && _gameState->getGameFlag(79)) {
			return 0;
		}
	} else if (locationId == 0xd) {
		if (param == 0x2f && _gameState->getGameFlag(81)) {
			return 0;
		}
		if (param == 48 && _gameState->getGameFlag(81)) {
			return 0;
		}
	} else if (locationId == 0x10) {
		switch (param) {
		case 0x3e8:
			if (!(_gameState->_gameGlobalData[30] & 1))
				return 0;
			break;
		case 0x3e9:
			if (!(_gameState->_gameGlobalData[30] & 2))
				return 0;
			break;
		case 0x3ea:
			if (!(_gameState->_gameGlobalData[30] & 4))
				return 0;
			break;
		case 0x3eb:
			if (!(_gameState->_gameGlobalData[30] & 8))
				return 0;
			break;
		case 0x3ec:
			if (!(_gameState->_gameGlobalData[30] & 16))
				return 0;
			break;
		case 0x3ed:
			if (!(_gameState->_gameGlobalData[30] & 32))
				return 0;
			break;
		case 0x3ee:
			if (!(_gameState->_gameGlobalData[30] & 64))
				return 0;
			break;
		default:
			break;
		};
		return 1;
	} else if (locationId == 0x12) {
		if (param == 0x28 && _gameState->getGameFlag(91)) {
			return 0;
		}
		if (param == 41 && (!_gameState->getGameFlag(96) || _gameState->getGameFlag(91))) {
			return 0;
		}
	} else if (locationId == 0x13) {
		if (param == 0x32 && _gameState->getGameFlag(107)) {
			return 0;
		}
		if (param == 68 && !_gameState->getGameFlag(107)) {
			return 0;
		}
	} else if (locationId == 0x14) {
		if (param == 1000 && !_gameState->getGameFlag(82)) {
			return 0;
		}
	} else if (locationId == 0x25) {
		if (param == 7 && _gameState->_gameGlobalData[28] != 1) {
			return 0;
		}
		if (param == 8 && _gameState->_gameGlobalData[28] != 1) {
			return 0;
		}
		if (param == 9 && _gameState->_gameGlobalData[28] != 1) {
			return 0;
		}
		if (param == 75 && _gameState->_gameGlobalData[28] != 2) {
			return 0;
		}
	} else if (locationId == 72) {
		if (param == 63 && _gameState->getGameFlag(105)) {
			return 0;
		}
		if (param == 67 && !_gameState->getGameFlag(105)) {
			return 0;
		}
		if (param == 0x40 && !_gameState->getGameFlag(105)) {
			return 0;
		}
	}
	return 1;
}

int32 ToonEngine::runConversationCommand(int16 **command) {

	int16 *v5 = *command;

	int v2 = READ_LE_INT16(v5);
	int v4 = READ_LE_INT16(v5 + 1);
	int result = v2 - 100;
	switch (v2) {
	case 100:
		result = runEventScript(_mouseX, _mouseY, 2, v4, 0);
		break;
	case 101:
		_gameState->_exitConversation = true;
		break;
	case 102:
		playSoundWrong();
		break;
	case 104:
		*command = (int16 *)((char *)_conversationData + v4 - 4);
		break;
		//
	case 105:
		if (getConversationFlag(_gameState->_currentScene, v4)) {
			result = READ_LE_INT16(*command + 2);
			*command = (int16 *)((char *)_conversationData + result - 4);
		} else {
			int16 *newPtr = *command + 1;
			*command = newPtr;
		}
		break;
	case 103:
	default:
		break;
	}
	return result;
}

int32 ToonEngine::waitTicks(int32 numTicks, bool breakOnMouseClick) {
	uint32 nextTime = _system->getMillis() + numTicks * _tickLength;
	while (_system->getMillis() < nextTime || numTicks == -1) {
		//if (!_animationSceneScriptRunFlag)
		//	break;
		updateAnimationSceneScripts(0);
		getMouseEvent();
		simpleUpdate();

		if (breakOnMouseClick && (_mouseButton & 0x2))
			break;
	}
	return 0;
}

void ToonEngine::renderInventory() {
	if (!_gameState->_inInventory)
		return;

	if (!_dirtyAll) {
		_inventoryPicture->drawWithRectList(*_mainSurface, 0, 0, 0, 0, _dirtyRects);
	} else {
		_inventoryPicture->draw(*_mainSurface, 0, 0, 0, 0);
		_dirtyRects.push_back(Common::Rect(0, 0, TOON_SCREEN_WIDTH, TOON_SCREEN_HEIGHT));
	}
	clearDirtyRects();

	// draw items on screen
	for (int32 i = 0; i < _gameState->_numInventoryItems; ++i) {
		int32 x = 57 * (i % 7) + 114;
		int32 y = ((9 * (i % 7)) & 0xf) + 56 * (i / 7) + 80;
		_inventoryIconSlots->drawFrame(*_mainSurface, i % 12, x + _gameState->_currentScrollValue, y);
		if (_gameState->_inventory[i])
			_inventoryIcons->drawFrame(*_mainSurface, _gameState->_inventory[i], x + _gameState->_currentScrollValue + 2, y + 2);
	}

	drawConversationLine();
	if (!_audioManager->voiceStillPlaying()) {
		_currentTextLineCharacterId = -1;
		_currentTextLine = 0;
		_currentTextLineId = -1;
	}

	if (_firstFrame) {
		copyToVirtualScreen(false);
		_firstFrame = false;
		fadeIn(5);
	}
	copyToVirtualScreen();
}

int32 ToonEngine::showInventory() {
	int32 oldScrollValue = _gameState->_currentScrollValue;

	delete _inventoryPicture;
	_inventoryPicture = new Picture(this);
	fadeOut(5);
	_inventoryPicture->loadPicture("SACK128.CPS");
	_inventoryPicture->setupPalette();
	dirtyAllScreen();

	if (_gameState->_mouseState >= 0) {
		setCursor(_gameState->_mouseState, true, -18, -14);

		// make sure we have a free spot
		if (!_gameState->hasItemInInventory(0)) {
			_gameState->_inventory[_gameState->_numInventoryItems] = 0;
			_gameState->_numInventoryItems++;
		}
	} else {
		setCursor(0);
	}

	_gameState->_inInventory = true;
	_gameState->_currentScrollValue = 0;

	int32 oldMouseButton = 0x3;
	int32 justPressedButton = 0;
	_firstFrame = true;

	while (!_shouldQuit) {
		getMouseEvent();

		justPressedButton = _mouseButton & ~oldMouseButton;
		oldMouseButton = _mouseButton;

		if (justPressedButton & 0x3) {
			// find out what object we're on
			int32 foundObj = -1;
			for (int32 i = 0; i < _gameState->_numInventoryItems; ++i) {
				int32 x = 57 * (i % 7) + 114;
				int32 y = ((9 * (i % 7)) & 0xf) + 56 * (i / 7) + 80;
				if (_mouseX >= (_gameState->_currentScrollValue + x - 6) &&
						_mouseX <= (_gameState->_currentScrollValue + x + 44 + 7) &&
						_mouseY >= y - 6 && _mouseY <= y + 50) {
					foundObj = i;
					break;
				}
			}

			if (justPressedButton & 0x1) {
				if (_gameState->_mouseState < 0) {
					if (foundObj >= 0) {
						// take an object
						int32 item = _gameState->_inventory[foundObj];

						int32 modItem = getSpecialInventoryItem(item);
						if (modItem) {
							if (modItem == -1) {
								_gameState->_mouseState = item;
								_gameState->_inventory[foundObj] = 0;
							} else {
								_gameState->_mouseState = modItem;
								if (!_gameState->hasItemInInventory(0)) {
									_gameState->_inventory[_gameState->_numInventoryItems] = 0;
									_gameState->_numInventoryItems++;
								}
							}

							setCursor(_gameState->_mouseState, true, -18, -14);
						}

					} else {
						break;
					}
				} else {
					if (foundObj >= 0 && _gameState->_inventory[foundObj] == 0) { // empty place
						_gameState->_inventory[foundObj] = _gameState->_mouseState;
						setCursor(0, false);
						_gameState->_mouseState = -1;
					} else if (foundObj >= 0 && _gameState->_inventory[foundObj] > 0) {
						if (!handleInventoryOnInventory(_gameState->_mouseState, _gameState->_inventory[foundObj]))
							playSoundWrong();
					} else {
						// quit the inventory mode with the icon
						break;
					}
				}

			} else if (justPressedButton & 0x2) { // right button
				if (foundObj >= 0) {
					// talk about the object
					if (!handleInventoryOnInventory(_gameState->_inventory[foundObj], -1))
						characterTalk(1000 + _gameState->_inventory[foundObj]);
				} else {
					// go out
					break;
				}
			}
		}

		renderInventory();
		_system->delayMillis(10);
	}

	_gameState->_currentScrollValue = oldScrollValue;
	_gameState->_inInventory = false;
	_mouseButton = 0;
	_lastMouseButton = 0x3;

	fadeOut(5);
	if (_gameState->_inCloseUp) {
		_gameState->_inCloseUp = false;
		flipScreens();
	} else if (_gameState->_inCutaway) {
		_currentCutaway->setupPalette();
		setupGeneralPalette();
	} else {
		_currentPicture->setupPalette();
		setupGeneralPalette();
	}
	flushPalette();
	dirtyAllScreen();
	_firstFrame = true;

	return 0;
}

void ToonEngine::getMouseEvent() {
	Common::EventManager *_event = _system->getEventManager();

	Common::Event event;
	while (_event->pollEvent(event) && !_shouldQuit) { }

	_mouseX = _event->getMousePos().x;
	_mouseY = _event->getMousePos().y;
	_mouseButton = _event->getButtonState();
}

void ToonEngine::drawSack() {
	if (_gameState->_sackVisible) {
		_inventoryIcons->drawFrame(*_mainSurface, 0, _gameState->_currentScrollValue, 356);
	}
}

void ToonEngine::addItemToInventory(int32 item) {

	if (item == 103 || item == 104 || item == 89 || item == 82) {
		// can't add that to inventory
		_gameState->_mouseState = -1;
		return;
	}

	if (item == 41) {
		// confiscated inventory
		for (int32 i = 0; i < _gameState->_numConfiscatedInventoryItems; ++i)
			addItemToInventory(_gameState->_confiscatedInventory[i]);

		_gameState->_numConfiscatedInventoryItems = 0;
		_gameState->_mouseState = -1;
		return;
	}

	for (int32 i = 0; i < _gameState->_numInventoryItems; ++i) {
		if (_gameState->_inventory[i] == 0) {
			_gameState->_inventory[i] = item;
			_gameState->_mouseState = -1;
			return;
		}
	}
	_gameState->_inventory[_gameState->_numInventoryItems] = item;
	_gameState->_numInventoryItems++;
	_gameState->_mouseState = -1;
}

void ToonEngine::createMouseItem(int32 item) {
	_gameState->_mouseState = item;
	setCursor(_gameState->_mouseState, true, -18, -14);
}

void ToonEngine::deleteMouseItem() {
	_gameState->_mouseState = -1;
	rearrangeInventory();
	setCursor(0);
}

void ToonEngine::showCutaway(const Common::Path &cutawayPicture) {
	_gameState->_inCutaway = true;
	delete _currentCutaway;
	_currentCutaway = nullptr;
	_currentCutaway = new Picture(this);
	if (cutawayPicture.empty()) {
		Common::String name = _gameState->_locations[_gameState->_currentScene]._cutaway;
		_currentCutaway->loadPicture(Common::Path(name + ".CPS"));
	} else {
		_currentCutaway->loadPicture(cutawayPicture);
	}
	_currentCutaway->setupPalette();
	_oldScrollValue = _gameState->_currentScrollValue;
	_gameState->_currentScrollValue = 0;
	dirtyAllScreen();
	flushPalette();
}

void ToonEngine::hideCutaway() {
	_gameState->_inCutaway = false;
	_gameState->_sackVisible = true;
	delete _currentCutaway;
	_gameState->_currentScrollValue = _oldScrollValue;
	_currentCutaway = nullptr;
	_currentPicture->setupPalette();
	dirtyAllScreen();
	flushPalette();
}

void ToonEngine::updateCharacters(int32 timeElapsed) {
	for (int32 i = 0; i < 8; ++i) {
		if (_characters[i] != nullptr) {
			_characters[i]->update(timeElapsed);
		}
	}
}

void ToonEngine::drawPalette() {
	for (int32 i = 0; i < 256; ++i) {
		int32 x = i % 32;
		int32 y = i / 32;
		_mainSurface->fillRect(Common::Rect(x * 16, y * 16, x * 16 + 16, y * 16 + 16), i);
	}
}

void ToonEngine::rearrangeInventory() {
	for (int32 i = 0; i < _gameState->_numInventoryItems; ++i) {
		if (_gameState->_inventory[i] == 0) {
			// move all the following items from one
			for (int32 j = i + 1; j < _gameState->_numInventoryItems; ++j) {
				_gameState->_inventory[j - 1] = _gameState->_inventory[j];
			}
			_gameState->_numInventoryItems--;
		}
	}
}

void ToonEngine::newGame() {

	if (_isDemo) {
		addItemToInventory(59);
		addItemToInventory(67);
		if (!_isEnglishDemo) {
			addItemToInventory(11);
			addItemToInventory(19);
		}
		loadScene(22);
		//loadScene(_gameState->_currentScene);
	} else {
		//loadScene(4);
		loadScene(_gameState->_currentScene);
	}
}

void ToonEngine::playSFX(int32 id, int32 volume) {
	if (id < 0)
		_audioManager->playSFX(-id + 1, volume, true);
	else
		_audioManager->playSFX(id , volume, false);
}

void ToonEngine::playSoundWrong() {
	_audioManager->playSFX(randRange(0,7), 128, true);
}

void ToonEngine::getTextPosition(int32 characterId, int32 *retX, int32 *retY) {
	if (characterId < 0)
		characterId = 0;

	// default position is the center of current screen
	*retX = _gameState->_currentScrollValue + 320;
	*retY = 70;

	// hardcoded special cases...
	if (characterId == 0) {
		// drew
		int32 x = _drew->getX();
		int32 y = _drew->getY();
		if (x >= _gameState->_currentScrollValue && x <= _gameState->_currentScrollValue + TOON_SCREEN_WIDTH) {
			if (!_gameState->_inCutaway && !_gameState->_inInventory) {
				*retX = x;
				*retY = y - ((_drew->getScale() * 256 / 1024) >> 1) - 45;
			}
		}
	} else if (characterId == 1) {
		// flux
		int16 x = _flux->getX();
		int16 y = _flux->getY();
		if (x >= _gameState->_currentScrollValue && x <= _gameState->_currentScrollValue + TOON_SCREEN_WIDTH) {
			if (!_gameState->_inCutaway) {
				*retX = x;
				*retY = y - ((_drew->getScale() * 100 / 1024) >> 1) - 30;
			}
		}
	} else if (characterId == 5 || characterId == 39) {
		*retX = 80;
		*retY = 120;
	} else if (characterId == 14) {
		*retX = 257;
		*retY = 132;
	} else if (characterId == 18) {
		*retX = 80;
		*retY = 180;
	} else if (characterId == 21) {
		*retX = 363;
		*retY = 193;
	} else if (characterId == 23) {
		*retX = 532;
		*retY = 178;
	} else if (characterId == 33) {
		*retX = 167;
		*retY = 172;
	} else {

		// more "standard" code by character
		Character *character = getCharacterById(characterId);
		if (character && !_gameState->_inCutaway) {
			if (character->getAnimationInstance()) {
				if (character->getX() >= _gameState->_currentScrollValue && character->getX() <= _gameState->_currentScrollValue + TOON_SCREEN_WIDTH) {
					int16 x1= 0, y1 = 0, x2 = 0, y2 = 0;
					character->getAnimationInstance()->getRect(&x1, &y1, &x2, &y2);
					*retX = (x1 + x2) / 2;
					*retY = y1;
				}
			}
		}
	}
}

Character *ToonEngine::getCharacterById(int32 charId) {
	for (int32 i = 0; i < 8; ++i) {
		if (_characters[i] != nullptr && _characters[i]->getId() == charId)
			return _characters[i];
	}
	return 0;
}

void ToonEngine::drawConversationLine() {
	if (_currentTextLine && _showConversationText) {
		_fontRenderer->setFontColorByCharacter(_currentTextLineCharacterId);
		_fontRenderer->setFont(_currentFont);
		_fontRenderer->renderMultiLineText(_currentTextLineX, _currentTextLineY, _currentTextLine, 0, *_mainSurface);
	}
}

void ToonEngine::drawCustomText(int16 x, int16 y, const char *line, Graphics::Surface *frame, byte color) {
	if (line) {
		byte col = color; // 0xce
		_fontRenderer->setFontColor(0, col, col);
		//_fontRenderer->setFontColorByCharacter(_currentTextLineCharacterId);
		_gameState->_currentScrollValue = 0;
		_fontRenderer->setFont(_currentFont);
		_fontRenderer->renderMultiLineText(x, y, line, 0, *frame);
	}
}

bool ToonEngine::showConversationText() const {
	return _showConversationText;
}

void ToonEngine::pauseEngineIntern(bool pause) {

	Engine::pauseEngineIntern(pause);

	static int32 pauseStart = 0;
	if (pause) {
		pauseStart = _system->getMillis();

	} else {
		_oldTimer = _system->getMillis();
		_oldTimer2 = _oldTimer;

		int32 diff = _oldTimer - pauseStart;

		// we have to add the difference between the start and the current time
		// to all "timer based" values.
		for (int32 i = 0; i < _gameState->_locations[_gameState->_currentScene]._numSceneAnimations; ++i) {
			_sceneAnimationScripts[i]._lastTimer += diff;
		}
		for (int32 i = 0; i < 8; ++i) {
			if (_characters[i] != nullptr) {
				_characters[i]->updateTimers(diff);
			}
		}

		_gameState->_timerTimeout[0] += diff;
		_gameState->_timerTimeout[1] += diff;
	}
}

bool ToonEngine::canSaveGameStateCurrently(Common::U32String *msg) {
	return !_gameState->_inMenu && !_gameState->_inInventory && !_gameState->_inConversation && !_gameState->_inCutaway && !_gameState->_mouseHidden && !_moviePlayer->isPlaying();
}

bool ToonEngine::canLoadGameStateCurrently(Common::U32String *msg) {
	return !_gameState->_inMenu && !_gameState->_inInventory && !_gameState->_inConversation && !_gameState->_inCutaway && !_gameState->_mouseHidden && !_moviePlayer->isPlaying();
}

Common::String ToonEngine::getSavegameName(int nr) {
	return _targetName + Common::String::format(".%03d", nr);
}

bool ToonEngine::saveGame(int32 slot, const Common::String &saveGameDesc) {
	int16 savegameId;
	Common::String savegameDescription;


	// NOTE The original game engine additionally saved in EACH saved game file:
	//      - volume levels for music, speech and SFX
	//      - muted state for music, speech and SFX
	//      - text speed
	//      - disabled subtitles (text off) -- but not font selection.
	//      ScummVM skips saving (and restoring) this per saved game file.
	//      Instead it keeps these settings persisted and synced with
	//      ScummVM's ConfMan volume levels, text speed, and subtitles settings.

	if (slot == -1) {
		GUI::SaveLoadChooser *dialog = new GUI::SaveLoadChooser(_("Save game:"), _("Save"), true);
		savegameId = dialog->runModalWithCurrentTarget();
		savegameDescription = dialog->getResultString();
		delete dialog;
	} else {
		savegameId = slot;
		if (!saveGameDesc.empty()) {
			savegameDescription = saveGameDesc;
		} else {
			savegameDescription = Common::String::format("Quick save #%d", (int)slot);
		}
	}

	if (savegameId < 0)
		return false; // dialog aborted

	Common::String savegameFile = getSavegameName(savegameId);
	Common::OutSaveFile *saveFile = _saveFileMan->openForSaving(savegameFile);
	if (!saveFile)
		return false;

	// save savegame header
	saveFile->writeSint32BE(TOON_SAVEGAME_VERSION);

	if (savegameDescription == "") {
		savegameDescription = "Untitled saved game";
	}

	saveFile->writeSint16BE(savegameDescription.size() + 1);
	saveFile->write(savegameDescription.c_str(), savegameDescription.size() + 1);

	Graphics::saveThumbnail(*saveFile);

	TimeDate curTime;
	_system->getTimeAndDate(curTime);

	uint32 saveDate = (curTime.tm_mday & 0xFF) << 24 | ((curTime.tm_mon + 1) & 0xFF) << 16 | ((curTime.tm_year + 1900) & 0xFFFF);
	uint16 saveTime = (curTime.tm_hour & 0xFF) << 8 | ((curTime.tm_min) & 0xFF);

	saveFile->writeUint32BE(saveDate);
	saveFile->writeUint16BE(saveTime);
	uint32 playTime = getTotalPlayTime();
	saveFile->writeUint32BE(playTime);

	// save global state
	_gameState->save(saveFile);
	_gameState->saveConversations(saveFile);
	_hotspots->save(saveFile);

	// save current time to be able to patch the time when loading
	saveFile->writeSint32BE(getOldMilli());

	// save script states
	for (int32 i = 0; i < 4; ++i) {
		_script->saveState(&_scriptState[i], saveFile);
	}

	// save animation script states
	for (int32 i = 0; i < state()->_locations[_gameState->_currentScene]._numSceneAnimations; ++i) {
		saveFile->writeByte(_sceneAnimationScripts[i]._active);
		saveFile->writeByte(_sceneAnimationScripts[i]._frozen);
		saveFile->writeSint32BE(_sceneAnimationScripts[i]._lastTimer);
		_script->saveState(&_sceneAnimationScripts[i]._state, saveFile);
	}

	// save scene animations
	for (int32 i = 0; i < 64; ++i) {
		_sceneAnimations[i].save(this, saveFile);
	}

	for (int32 i = 0; i < 8; ++i) {
		if (_characters[i] != nullptr) {
			saveFile->writeSByte(i);
			_characters[i]->save(saveFile);
		}
	}
	saveFile->writeSByte(-1);

	// save "command buffer"
	saveFile->writeSint16BE(_saveBufferStream->pos());
	if (_saveBufferStream->pos() > 0) {
		saveFile->write(_saveBufferStream->getData(), _saveBufferStream->pos());
		saveFile->writeSint16BE(0);
	}

	delete saveFile;

	return true;
}

bool ToonEngine::loadGame(int32 slot) {
	int16 savegameId;

	if (slot == -1) {
		GUI::SaveLoadChooser *dialog = new GUI::SaveLoadChooser(_("Restore game:"), _("Restore"), false);
		savegameId = dialog->runModalWithCurrentTarget();
		delete dialog;
	} else {
		savegameId = slot;
	}
	if (savegameId < 0)
		return false; // dialog aborted

	Common::String savegameFile = getSavegameName(savegameId);
	Common::InSaveFile *loadFile = _saveFileMan->openForLoading(savegameFile);
	if (!loadFile)
		return false;

	int32 saveGameVersion = loadFile->readSint32BE();
	if ( (saveGameVersion < 4) || (saveGameVersion > TOON_SAVEGAME_VERSION) ) {
		delete loadFile;
		return false;
	}
	int32 saveGameNameSize = loadFile->readSint16BE();
	loadFile->skip(saveGameNameSize);

	// We don't need the thumbnail here, so just read it and discard it
	Graphics::skipThumbnail(*loadFile);

	loadFile->skip(6); // date & time skip

	uint32 playTimeMsec = 0;
	if (saveGameVersion >= 5) {
		playTimeMsec = loadFile->readUint32BE();
	}
	setTotalPlayTime(playTimeMsec);

	if (_gameState->_currentScene != -1) {
		exitScene();
	}

	_gameState->load(loadFile);
	loadScene(_gameState->_currentScene, true);
	_gameState->loadConversations(loadFile);
	_hotspots->load(loadFile);

	// read the old time
	int32 savedTime = loadFile->readSint32BE();
	int32 timerDiff = _system->getMillis() - savedTime;

	// load script states
	for (int32 i = 0; i < 4; ++i) {
		_script->loadState(&_scriptState[i], loadFile);
	}

	// load animation script states
	for (int32 i = 0; i < state()->_locations[_gameState->_currentScene]._numSceneAnimations; ++i) {
		_sceneAnimationScripts[i]._active = loadFile->readByte();
		_sceneAnimationScripts[i]._frozen = loadFile->readByte();
		_sceneAnimationScripts[i]._frozenForConversation = false;
		int32 oldTimer = loadFile->readSint32BE();
		_sceneAnimationScripts[i]._lastTimer = MAX<int32>(0, oldTimer + timerDiff);
		_script->loadState(&_sceneAnimationScripts[i]._state, loadFile);
	}

	// load scene animations
	for (int32 i = 0; i < 64; ++i) {
		_sceneAnimations[i].load(this, loadFile);
	}

	// scene animations have to be added in reverse order in animation manager to preserve the z order
	for (int32 i = 63; i >= 0; --i) {
		if (_sceneAnimations[i]._active && _sceneAnimations[i]._animInstance) {
			_animationManager->addInstance(_sceneAnimations[i]._animInstance);
		}
	}

	_gameState->_timerTimeout[0] += timerDiff;
	_gameState->_timerTimeout[1] += timerDiff;

	_gameState->_conversationData = _conversationData;
	_firstFrame = true;

	// read characters info
	while (1) {
		int8 c = loadFile->readSByte();
		if (c < 0)
			break;

		// NOTE The exitScene() call above removes characters in scene (if any current exists) (and only for the 8 (scene?) characters)
		if (_characters[c] == nullptr) {
			_characters[c] = new Character(this);
		}

		_characters[c]->load(loadFile, saveGameVersion);
		// NOTE These playStandingAnim() calls are executed in the loadScene() call above,
		// but that's before we're loading stuff like the _facing of actors
		if (_characters[c] == _drew) {
			_drew->playStandingAnim();
		} else if (_characters[c] == _flux && _gameState->_currentChapter == 1) {
			_flux->playStandingAnim();
		}
		//_characters[c]->setVisible(true);
		_characters[c]->update(0);
	}

	// load "command buffer"
	int32 size = loadFile->readSint16BE();
	if (size) {
		uint8 *buf = new uint8[size + 2];
		loadFile->read(buf, size + 2);

		Common::MemoryReadStream rStr(buf, size + 2);
		while (1) {
			int16 command = rStr.readSint16BE();
			if (!command) break;
			switch (command) {
			case 1: {
				int16 frame = rStr.readSint16BE();
				int16 animLen = rStr.readSint16BE();
				char animName[32];
				rStr.read(animName, animLen);
				int16 x = rStr.readSint16BE();
				int16 y = rStr.readSint16BE();
//				int16 z = rStr.readSint16BE();
//				int16 layerZ = rStr.readSint16BE();
				rStr.readSint16BE();
				rStr.readSint16BE();

				Animation *anim = new Animation(this);
				anim->loadAnimation(animName);
				anim->drawFrameOnPicture(frame, x, y);
				delete anim;
				break;
			}
			case 2: {
				int16 x =  rStr.readSint16BE();
				int16 y = rStr.readSint16BE();
				int16 x1 = rStr.readSint16BE();
				int16 y1 = rStr.readSint16BE();
				makeLineNonWalkable(x, y, x1, y1);
				break;
			}
			case 3: {
				int16 x =  rStr.readSint16BE();
				int16 y = rStr.readSint16BE();
				int16 x1 = rStr.readSint16BE();
				int16 y1 = rStr.readSint16BE();
				makeLineWalkable(x, y, x1, y1);
				break;
			}
			case 4: {
				int16 x = rStr.readSint16BE();
				int16 y = rStr.readSint16BE();
				getMask()->floodFillNotWalkableOnMask(x, y);
				break;
			}
			default:
				break;
			}
		}
		_saveBufferStream->write(buf, size);
		delete[] buf;
	}
	delete loadFile;

	// setup correct palette if we are in a closeup/cutaway or not.
	if (_gameState->_inCloseUp) {
		_gameState->_inCloseUp = false;
		flipScreens();
	} else if (_gameState->_inCutaway) {
		_currentCutaway->setupPalette();
		setupGeneralPalette();
	} else {
		_currentPicture->setupPalette();
		setupGeneralPalette();
	}
	flushPalette();

	return true;
}

// another special case for inventory
int32 ToonEngine::getSpecialInventoryItem(int32 item) {
	// butter
	if (item == 12) {
		for (int32 i = 0; i < _gameState->_numInventoryItems; ++i) {
			if (_gameState->_inventory[i] == 12)
				_gameState->_inventory[i] = 11;
		}
		return 11;

	} else if (item == 84) {
		if (_gameState->getGameFlag(26)) {
			characterTalk(1726);
			return 0;
		} else {
			if (!_gameState->hasItemInInventory(102) && !_gameState->hasItemInInventory(90) && !_gameState->hasItemInInventory(89)) {
				characterTalk(1416);
				return 102;
			} else {
				return 0;
			}
		}
	}

	return -1;
}

void ToonEngine::initCharacter(int32 characterId, int32 animScriptId, int32 sceneAnimationId, int32 animToPlayId) {
	// find a new index
	int32 characterIndex = -1;
	for (int32 i = 0; i < 8; ++i) {
		if (_characters[i] != nullptr && _characters[i]->getId() == characterId) {
			// TODO Shouldn't _characters[i] be deleted here,
			// since we assign a new Character object to it below?
			characterIndex = i;
			break;
		}

		if (_characters[i] == nullptr) {
			characterIndex = i;
			break;
		}
	}

	if (characterIndex == -1) {
		return;
	}

	_characters[characterIndex] = new Character(this);
	_characters[characterIndex]->setId(characterId);
	_characters[characterIndex]->setAnimScript(animScriptId);
	_characters[characterIndex]->setDefaultSpecialAnimationId(animToPlayId);
	_characters[characterIndex]->setSceneAnimationId(sceneAnimationId);
	_characters[characterIndex]->setFlag(0);
	_characters[characterIndex]->setVisible(true);
	if (sceneAnimationId != -1)
		_characters[characterIndex]->setAnimationInstance(_sceneAnimations[sceneAnimationId]._animInstance);
}

int32 ToonEngine::handleInventoryOnFlux(int32 itemId) {

	switch (itemId) {
	case 8:
		sayLines(1, 1332);
		break;
	case 0x14:
	case 0x15:
	case 0x45:
		sayLines(1, 1304);
		break;
	case 0x68:
		_gameState->_mouseState = 0;
		setCursor(0, false, 0, 0);
		break;
	case 116:
		sayLines(1, 1306);
		break;
	default:
		return false;
	}
	return true;
}

void ToonEngine::storePalette() {
	memcpy(_backupPalette, _finalPalette, 768);
}

void ToonEngine::restorePalette() {
	memcpy(_finalPalette, _backupPalette, 768);
	flushPalette();
}

const char *ToonEngine::getSpecialConversationMusic(int32 conversationId) {
	static const char * const specialMusic[] = {
		0, 0,
		"BR091013", "BR091013",
		"NET1214", "NET1214",
		0, 0,
		"CAR1365B", "CAR1365B",
		0, 0,
		0, 0,
		"CAR14431", "CAR14431",
		0, 0,
		0, 0,
		"SCD16520", "SCD16520",
		"SCD16520", "SCD16520",
		"SCD16522", "SCD16522",
		0, 0,
		"KPM8719", "KPM8719",
		0, 0,
		"CAR1368B", "CAR1368B",
		0, 0,
		0, 0,
		"KPM6337", "KPM6337",
		"CAR20471", "CAR20471",
		"CAR136_1", "KPM87_57",
		0, 0,
		"CAR13648", "CAR13648",
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		"SCD16526", "SCD16526",
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0
	};

	return specialMusic[randRange(0, 1) + conversationId * 2];
}

void ToonEngine::viewInventoryItem(const Common::Path &str, int32 lineId, int32 itemDest) {
	storePalette();
	fadeOut(5);

	Picture *pic = new Picture(this);
	pic->loadPicture(str);
	pic->setupPalette();
	dirtyAllScreen();
	flushPalette();

	if (lineId) {
		characterTalk(lineId, false);
	}

	uint32 oldMouseButton = _mouseButton;
	uint32 justPressedButton = 0;
	_firstFrame = true;

	int32 oldScrollValue = _gameState->_currentScrollValue;
	_gameState->_currentScrollValue = 0;

	while (!_shouldQuit) {
		getMouseEvent();

		justPressedButton = _mouseButton & ~oldMouseButton;
		oldMouseButton = _mouseButton;

		if (justPressedButton) {
			break;
		}

		if (!_dirtyAll) {
			pic->drawWithRectList(*_mainSurface, 0, 0, 0, 0, _dirtyRects);
		} else {
			pic->draw(*_mainSurface, 0, 0, 0, 0);
			_dirtyRects.push_back(Common::Rect(0, 0, TOON_SCREEN_WIDTH, TOON_SCREEN_HEIGHT));
		}
		clearDirtyRects();

		drawConversationLine();
		if (!_audioManager->voiceStillPlaying()) {
			_currentTextLineCharacterId = -1;
			_currentTextLine = 0;
			_currentTextLineId = -1;
		}

		if (_firstFrame) {
			copyToVirtualScreen(false);
			_firstFrame = false;
			fadeIn(5);
		}

		copyToVirtualScreen();
	}

	fadeOut(5);
	dirtyAllScreen();
	restorePalette();
	_firstFrame = true;
	_gameState->_currentScrollValue = oldScrollValue;
	delete pic;
}

int32 ToonEngine::handleInventoryOnInventory(int32 itemDest, int32 itemSrc) {
	switch (itemDest) {
	case 0:
		return handleInventoryOnDrew(itemSrc);
	case 1:
		if (itemSrc == 71) {
			sayLines(2, 1212);
			return 1;
		}
		break;
	case 5:
		if (itemSrc == 15) {
			characterTalk(1492);
		} else if (itemSrc == 0x2f) {
			characterTalk(1488);
		} else if (itemSrc == 88) {
			sayLines(2, 1478);
		} else {
			return 0;
		}
		break;
	case 6:
		if (itemSrc == -1) {
			viewInventoryItem("BLUEPRNT.CPS", 1006, itemDest);
			return 1;
		} else
			return 0;
		break;
	case 8:
		if (itemSrc == -1) {
			viewInventoryItem("BOOK.CPS", 0, itemDest);
			return 1;
		} else {
			return 0;
		}
		break;
	case 11:
		if (itemSrc == 0xb) {
			_gameState->_mouseState = -1;
			replaceItemFromInventory(11, 12);
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
			//
		} else if (itemSrc == 24) {
			characterTalk(1244);
			return 1;
		} else if (itemSrc == 0x1a || itemSrc == 0x40 || itemSrc == 71) {
			sayLines(2, 1212);
			return 1;
		}
		break;
	case 12:
		if (itemSrc == 24) {
			characterTalk(1244);
			return 1;
		} else if (itemSrc == 0x1a || itemSrc == 0x40 || itemSrc == 71) {
			sayLines(2, 1212);
			return 1;
		}
		break;
	case 13:
		if (itemSrc == 0x35 || itemSrc == 0x36) {
			characterTalk(1204);
			return 1;
		} else if (itemSrc >= 0x6b && itemSrc <= 0x72) {
			characterTalk(1312);
			return 1;
		}
		break;
	case 14:
		if (itemSrc == -1) {
			deleteItemFromInventory(14);
			addItemToInventory(15);
			addItemToInventory(42);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 43) {
			characterTalk(1410);
			return 1;
		} else if (itemSrc == 49) {
			characterTalk(1409);
			return 1;
		}
		break;
	case 16:
		if (itemSrc == 55) {
			characterTalk(1400);
			replaceItemFromInventory(55, 98);
			return 1;
		}
		break;
	case 19:
		if (itemSrc == 0x34) {
			characterTalk(1322);
			return 1;
		} else if (itemSrc == 107) {
			sayLines(2 , 1300);
			replaceItemFromInventory(107, 111);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 0x6c) {
			sayLines(2, 1300);
			replaceItemFromInventory(108, 112);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 0x6d) {
			sayLines(2, 1300);
			replaceItemFromInventory(109, 113);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 110) {
			sayLines(2, 1300);
			replaceItemFromInventory(110, 114);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		}
		break;
	case 20:
		if (itemSrc == 35) {
			createMouseItem(21);
			replaceItemFromInventory(35, 36);
			return 1;
		} else if (itemSrc == 0x24) {
			createMouseItem(21);
			replaceItemFromInventory(36, 37);
			return 1;
		} else if (itemSrc == 37) {
			deleteItemFromInventory(37);
			createMouseItem(21);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 0x6b || itemSrc == 0x6c || itemSrc == 0x6f || itemSrc == 0x70) {
			sayLines(2, 1292);
			return 1;
		}
		break;
	case 21:
		switch (itemSrc) {
		case 107:
			characterTalk(1296);
			replaceItemFromInventory(107, 109);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		case 108:
			characterTalk(1298);
			replaceItemFromInventory(108, 110);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		case 111:
			characterTalk(1296);
			replaceItemFromInventory(111, 113);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		case 112:
			characterTalk(1298);
			replaceItemFromInventory(112, 114);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		default:
			break;
		}
		break;
	case 22:
		if (itemSrc == 32) {
			characterTalk(1252);
			return 1;
		}
		break;
	case 24:
		if (itemSrc == 0xc) {
			characterTalk(1244);
			return 1;
		} else if (itemSrc == 79) {
			characterTalk(1280);
			return 1;
		}
		break;
	case 26:
		if (itemSrc == 0x5e) {
			characterTalk(1316);
			return 1;
		} else if (itemSrc == 95) {
			characterTalk(1320);
			return 1;
		}
		break;
	case 31:
		if (itemSrc == 61) {
			characterTalk(1412);
			deleteItemFromInventory(61);
			createMouseItem(62);
			rearrangeInventory();
			return 1;
		}
		break;
	case 32:
		if (itemSrc == 22) {
			characterTalk(1252);
			return 1;
		}
		break;
	case 33:
		if (itemSrc == 117) {
			characterTalk(1490);
			return 1;
		}
		break;
	case 34:
		if (itemSrc == 61) {
			characterTalk(1414);
			return 1;
		}
		break;
	case 35:
		if (itemSrc == -1) {
			characterTalk(1035);
			return 1;
		} else if (itemSrc == 20) {
			replaceItemFromInventory(20, 21);
			createMouseItem(36);
			return 1;
		} else if (itemSrc == 68) {
			replaceItemFromInventory(68, 69);
			createMouseItem(36);
			return 1;
		} else if (itemSrc >= 107 && itemSrc <= 114) {
			characterTalk(1314);
			return 1;
		} else {
			characterTalk(1208);
			return 1;
		}
		break;
	case 36:
		if (itemSrc == -1) {
			characterTalk(1035);
			return 1;
		} else if (itemSrc == 20) {
			replaceItemFromInventory(20, 21);
			createMouseItem(37);
			return 1;
		} else if (itemSrc == 68) {
			replaceItemFromInventory(68, 69);
			createMouseItem(37);
			return 1;
		} else if (itemSrc >= 107 && itemSrc <= 114) {
			characterTalk(1314);
			return 1;
		} else {
			characterTalk(1208);
			return 1;
		}
		break;
	case 37:
		if (itemSrc == -1) {
			characterTalk(1035);
			return 1;
		} else if (itemSrc == 20) {
			replaceItemFromInventory(20, 21);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 68) {
			replaceItemFromInventory(68, 69);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		} else if (itemSrc >= 107 && itemSrc <= 114) {
			characterTalk(1314);
			return 1;
		} else {
			characterTalk(1208);
			return 1;
		}
		break;
	case 38:
		if (itemSrc == 15) {
			characterTalk(1492);
			return 1;
		} else if (itemSrc == 0x2f) {
			characterTalk(1488);
			return 1;
		} else if (itemSrc == 88) {
			sayLines(2, 1478);
			return 1;
		}
		break;
	case 40:
		if (itemSrc == 53) {
			replaceItemFromInventory(53, 54);
			characterTalk(1222);
			return 1;
		} else if (itemSrc == 0x36) {
			characterTalk(1228);
			return 1;
		} else if (itemSrc == 0x5b) {
			characterTalk(1230);
			return 1;
		} else if (itemSrc == 92) {
			characterTalk(1220);
			return 1;
		}
		break;
	case 43:
		if (itemSrc == 14) {
			characterTalk(1410);
			return 1;
		}
		break;
	case 47:
		if (itemSrc == -1)
			characterTalk(1047);
		else
			characterTalk(1488);

		return 1;
	case 49:
		if (itemSrc == 0xe) {
			characterTalk(1409);
			return 1;
		} else if (itemSrc == 38 || itemSrc == 5 || itemSrc == 0x42) {
			characterTalk(1476);
			return 1;
		} else if (itemSrc == 0x34) {
			characterTalk(1260);
			return 1;
		} else if (itemSrc == 0x47) {
			characterTalk(1246);
			return 1;
		} else if (itemSrc == 0x36) {
			sayLines(2, 1324);
			return 1;
		}
		break;
	case 52:
		if (itemSrc == 0x13) {
			characterTalk(1322);
			return 1;
		} else if (itemSrc == 94) {
			characterTalk(1282);
			return 1;
		}
		break;
	case 53:
		if (itemSrc == 40) {
			createMouseItem(54);
			characterTalk(1222);
			return 1;
		} else if (itemSrc == 0x31) {
			sayLines(2, 1324);
			return 1;
		} else if (itemSrc == 0x34) {
			characterTalk(1310);
			return 1;
		} else if (itemSrc == 91) {
			characterTalk(1218);
			return 1;
		}

		break;
	case 54:
		if (itemSrc == 40) {
			characterTalk(1228);
			return 1;
		} else if (itemSrc == 0x34) {
			characterTalk(1310);
			return 1;
		} else if (itemSrc == 0x5b) {
			characterTalk(1226);
			replaceItemFromInventory(91, 92);
			return 1;
		} else if (itemSrc == 92) {
			characterTalk(1220);
			return 1;
		}

		break;
	case 55:
		if (itemSrc == 16) {
			createMouseItem(98);
			characterTalk(1400);
			return 1;
		}
		break;
	case 61:
		if (itemSrc == 0x1f) {
			characterTalk(1412);
			deleteItemFromInventory(31);
			createMouseItem(62);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 0x21 || itemSrc == 0x22) {
			characterTalk(1414);
			return 1;
		}
		break;
	case 64:
		if (itemSrc == 0xb) {
			sayLines(2, 1212);
			return 1;
		} else if (itemSrc == 0x5e || itemSrc == 0x5f) {
			characterTalk(1318);
			return 1;
		}
		break;
	case 66:
		if (itemSrc == 15) {
			characterTalk(1492);
			return 1;
		} else if (itemSrc == 0x2f) {
			characterTalk(1488);
			return 1;
		} else if (itemSrc == 88) {
			sayLines(2, 1478);
			characterTalk(1478);
			return 1;
		}
		break;
	case 67:
		if (itemSrc == 79) {
			sayLines(2, 1212);
			return 1;
		}
		break;
	case 68:
		if (itemSrc == 35) {
			createMouseItem(69);
			replaceItemFromInventory(35, 36);
			return 1;
		} else if (itemSrc == 0x24) {
			createMouseItem(69);
			replaceItemFromInventory(36, 37);
			return 1;
		} else if (itemSrc == 37) {
			deleteItemFromInventory(37);
			createMouseItem(69);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 0x6b || itemSrc == 113 || itemSrc == 0x6f || itemSrc == 109) {
			sayLines(2, 1288);
			return 1;
		}
		break;
	case 69:
		switch (itemSrc) {
		case 107:
			characterTalk(1296);
			replaceItemFromInventory(107, 108);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		case 109:
			characterTalk(1298);
			replaceItemFromInventory(109, 110);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		case 111:
			characterTalk(1296);
			replaceItemFromInventory(111, 112);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		case 113:
			characterTalk(1298);
			replaceItemFromInventory(113, 114);
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			return 1;
		default:
			break;
		}
		break;
	case 71:
		if (itemSrc == 0xc || itemSrc == 1 || itemSrc == 0x41 || itemSrc == 67 || itemSrc == 0x4c || itemSrc == 57) {
			sayLines(2, 1212);
			return 1;
		} else if (itemSrc == 79) {
			characterTalk(1238);
			return 1;
		}
		break;
	case 79:
		if (itemSrc == 1 || itemSrc == 67 || itemSrc == 76 || itemSrc == 57 || itemSrc == 0x41) {
			sayLines(2, 1212);
			return 1;
		} else if (itemSrc == 0x18) {
			characterTalk(1280);
			return 1;
		} else if (itemSrc == 0x47) {
			characterTalk(1238);
			return 1;
		}
		break;
	case 82:
		if (itemSrc == 84) {
			sayLines(2, 1424);
			return 1;
		} else if (itemSrc == 0x58) {
			deleteItemFromInventory(88);
			createMouseItem(89);
			rearrangeInventory();
			characterTalk(1428);
			return 1;
		} else if (itemSrc == 117) {
			sayLines(2, 1496);
			return 1;
		}
		break;
	case 84:
		if (itemSrc == 0x58) {
			replaceItemFromInventory(88, 90);
			characterTalk(1090);
			return 1;
		} else if (itemSrc == 117) {
			characterTalk(1494);
			return 1;
		}
		break;
	case 88:
		if (itemSrc == 82) {
			deleteItemFromInventory(82);
			createMouseItem(89);
			rearrangeInventory();
			characterTalk(1428);
			return 1;
		} else if (itemSrc == 0x54) {
			createMouseItem(90);
			characterTalk(1090);
			return 1;
		} else if (itemSrc == 102) {
			deleteItemFromInventory(102);
			createMouseItem(90);
			rearrangeInventory();
			characterTalk(1090);
			return 1;
		}
		break;
	case 89:
		if (itemSrc == 117) {
			sayLines(2, 1496);
			return 1;
		}
		break;
	case 90:
		if (itemSrc == 117) {
			sayLines(2, 1494);
			return 1;
		}
		break;
	case 91:
		if (itemSrc == 0x28) {
			characterTalk(1230);
			return 1;
		} else if (itemSrc == 54) {
			createMouseItem(92);
			return 1;
		}
		break;
	case 92:
		if (itemSrc == 0x28 || itemSrc == 54) {
			characterTalk(1220);
			return 1;
		}
		break;
	case 94:
		if (itemSrc == 26) {
			characterTalk(1316);
			return 1;
		} else if (itemSrc == 0x34) {
			characterTalk(1282);
			return 1;
		} else if (itemSrc == 64) {
			characterTalk(1318);
			return 1;
		}
		break;
	case 95:
		if (itemSrc == 26) {
			characterTalk(1320);
			return 1;
		} else if (itemSrc == 0x40) {
			characterTalk(1318);
			return 1;
		} else if (itemSrc == 115) {
			characterTalk(1284);
			replaceItemFromInventory(115, 116);
			createMouseItem(93);
			return 1;
		}
		break;
	case 96:
		if (itemSrc == 0x34) {
			characterTalk(1234);
			return 1;
		} else if (itemSrc == 71) {
			sayLines(2, 1212);
			return 1;
		}
		break;
	case 97:
		if (itemSrc == 15) {
			characterTalk(1492);
			return 1;
		} else if (itemSrc == 0x2f) {
			characterTalk(1488);
			return 1;
		} else if (itemSrc == 88) {
			sayLines(2, 1478);
			return 1;
		}
		break;
	case 100:
		if (itemSrc == 117) {
			characterTalk(1490);
			return 1;
		}
		break;
	case 102:
		if (itemSrc == -1) {
			characterTalk(1102);
			return 1;
		} else if (itemSrc == 84) {
			_gameState->_mouseState = -1;
			setCursor(0, false, 0, 0);
			rearrangeInventory();
			characterTalk(1418);
			return 1;
		} else if (itemSrc == 88) {
			deleteItemFromInventory(88);
			createMouseItem(90);
			rearrangeInventory();
			characterTalk(1090);
			return 1;
		} else if (itemSrc == 117) {
			characterTalk(1494);
			return 1;
		} else {
			characterTalk(1426);
			return 1;
		}
		break;
	case 106:
		if (itemSrc == 13) {
			characterTalk(1308);
			return 1;
		}
		break;
	case 107:
		if (itemSrc == 19) {
			sayLines(2, 1300);
			deleteItemFromInventory(19);
			createMouseItem(111);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 0x15) {
			characterTalk(1296);
			deleteItemFromInventory(21);
			createMouseItem(109);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 0x23) {
			characterTalk(1314);
			return 1;
		} else if (itemSrc == 69) {
			characterTalk(1296);
			deleteItemFromInventory(69);
			createMouseItem(108);
			rearrangeInventory();
			return 1;
		}
		break;
	case 108:
		if (itemSrc == 19) {
			sayLines(2, 1300);
			deleteItemFromInventory(19);
			createMouseItem(112);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 0x15) {
			characterTalk(1298);
			deleteItemFromInventory(21);
			createMouseItem(110);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 35) {
			characterTalk(1314);
			return 1;
		}
		break;
	case 109:
		if (itemSrc == 19) {
			sayLines(2, 1300);
			deleteItemFromInventory(19);
			createMouseItem(113);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 0x23) {
			characterTalk(1314);
			return 1;
		} else if (itemSrc == 69) {
			characterTalk(1298);
			deleteItemFromInventory(69);
			createMouseItem(110);
			rearrangeInventory();
			return 1;
		}
		break;
	case 110:
		if (itemSrc == 0x13) {
			sayLines(2, 1300);
			deleteItemFromInventory(19);
			createMouseItem(114);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 35) {
			characterTalk(1314);
			return 1;
		}
		break;
	case 111:
		if (itemSrc == 21) {
			characterTalk(1296);
			deleteItemFromInventory(21);
			createMouseItem(113);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 0x23) {
			characterTalk(1314);
			return 1;
		} else if (itemSrc == 69) {
			characterTalk(1296);
			deleteItemFromInventory(69);
			createMouseItem(112);
			rearrangeInventory();
			return 1;
		}
		break;
	case 112:
		if (itemSrc == 0x15) {
			characterTalk(1298);
			deleteItemFromInventory(21);
			createMouseItem(114);
			rearrangeInventory();
			return 1;
		} else if (itemSrc == 35) {
			characterTalk(1314);
			return 1;
		}
		break;
	case 113:
		if (itemSrc == 0x23) {
			characterTalk(1314);
			return 1;
		} else if (itemSrc == 69) {
			characterTalk(1298);
			deleteItemFromInventory(69);
			createMouseItem(114);
			rearrangeInventory();
			return 1;
		}
		break;
	case 114:
		if (itemSrc == 35) {
			characterTalk(1314);
			return 1;
		}
		break;
	case 115:
		if (itemSrc == 95) {
			replaceItemFromInventory(95, 93);
			createMouseItem(116);
			return 1;
		}
		break;
	case 117:
		if (itemSrc == 90 || itemSrc == 33) {
			characterTalk(1490);
		} else if (itemSrc == 102 || itemSrc == 84) {
			characterTalk(1494);
		} else if (itemSrc == 0x59 || itemSrc == 0x52) {
			characterTalk(1496);
		}
		break;
	default:
		break;
	}
	return 0;
}
int32 ToonEngine::handleInventoryOnDrew(int32 itemId) {
	switch (itemId) {
	case 1:
		sayLines(1, 1232);
		return 1;
	case 2:
		sayLines(2, 1202);
		return 1;
	case 7:
		if (_gameState->_currentScene == 32) {
			runEventScript(_mouseX, _mouseY, 2, 107, 0);
		} else if (_gameState->_currentScene < 37) {
			sayLines(2, 1258);
		} else {
			sayLines(2, 1462);
		}
		return 1;
	case 8:
		sayLines(2, 1328);
		return 1;
	case 0xc:
		sayLines(1, 1266);
		return 1;
	case 0xd:
		sayLines(1, 1206);
		return 1;
	case 16:
		sayLines(1, 1438);
		return 1;
	case 0x12:
		if (_gameState->_currentScene == 30) {
			runEventScript(_mouseX, _mouseY, 2, 106, 0);
			_gameState->_mouseState = -1;
		} else {
			sayLines(2, 1200);
		}
		return 1;
	case 0x14:
		sayLines(1, 1216);
		return 1;
	case 22:
		if (_gameState->_currentScene != 39 && _gameState->_currentScene != 50 && _gameState->_currentScene != 49) {
			if (_gameState->_currentScene < 37) {
				sayLines(1, 1256);
			} else {
				sayLines(1, 1456);
			}
		} else {
			runEventScript(_mouseX, _mouseY, 2, 100 , 0);
		}
		return 1;
	case 0x18:
		sayLines(1, 1216);
		return 1;
	case 0x23:
		sayLines(1, 1210);
		return 1;
	case 0x31:
		sayLines(1, 1262);
		return 1;
	case 50:
		if (_gameState->_currentScene == 37) {
			runEventScript(_mouseX, _mouseY, 2, 103, 0);
			return 1;
		};
		break;
	case 0x36:
		if (_gameState->_currentScene == 46) {
			runEventScript(_mouseX, _mouseY, 2, 102, 0);
		} else {
			sayLines(1, 1224);
		}
		return 1;
	case 0x37:
		sayLines(1, 1408);
		return 1;
	case 0x20:
		sayLines(1, 1254);
		return 1;
	case 0x21:
		sayLines(1, 1268);
		return 1;
	case 0x22:
		if (_gameState->_currentScene == 52) {
			runEventScript(_mouseX, _mouseY, 2, 104, 0);
			return 1;
		} else {
			_gameState->_mouseHidden = true;
			_drew->setFacing(4);
			sayLines(1, 1465);
			sayLines(1, randRange(0, 1) + 1468);
			createMouseItem(33);
			_gameState->_mouseHidden = false;
			return 1;
		}
		break;
	case 31:
		sayLines(1, 1436);
		return 1;
	case 0x1a:
		sayLines(1, 1216);
		return 1;
	case 0x39:
		sayLines(1, 1270);
		return 1;
	case 0x3a:
		sayLines(1, 1444);
		return 1;
	case 0x3b:
		sayLines(1, 1272);
		return 1;
	case 0x3f:
		if (_gameState->_currentScene != 10 && _gameState->_currentScene != 30 && _gameState->_currentScene != 22) {
			sayLines(1, 1274);
		} else {
			runEventScript(_mouseX, _mouseY, 2, 109, 0);
		}
		return 1;
	case 0x41:
		sayLines(1, 1232);
		return 1;

	case 0x4b:
		if (_gameState->_currentScene != 53) {
			_gameState->_mouseHidden = true;
			_drew->setFacing(4);
			sayLines(1, 1437);
			sayLines(2, 1440);
			_gameState->_mouseHidden = false;
		} else {
			runEventScript(_mouseX, _mouseY, 2 , 101, 0);
		}
		return 1;
	case 79:
		sayLines(1, 1242);
		return 1;
	case 0x4c:
		sayLines(1, 1232);
		return 1;
	case 71:
		sayLines(1, 1250);
		return 1;
	case 0x43:
		sayLines(1, 1216);
		return 1;
	case 0x60:
		sayLines(2, 1236);
		return 1;
	case 99:
		if (_gameState->_currentScene == 43) {
			runEventScript(_mouseX, _mouseY, 2, 105, 0);
		}
		_gameState->_mouseState = -1;
		setCursor(0, false, 0, 0);
		sayLines(1, 1555);
		return 1;
	case 0x5a:
		sayLines(1, 1432);
		return 1;
	case 0x58:
		sayLines(1, 1432);
		return 1;
	case 0x65:
		if (_gameState->_currentScene == 52) {
			runEventScript(_mouseX, _mouseY, 2, 104, 0);
		} else {
			_gameState->_mouseHidden = true;
			_drew->setFacing(4);
			sayLines(1, 1464);
			sayLines(1, 1468 + randRange(0, 1));
			createMouseItem(100);
			_gameState->_mouseHidden = false;
		}
		return 1;
	case 0x74:
		sayLines(1, 1286);
		return 1;
	case 0x75:
		sayLines(1, 1482);
		return 1;
	case 118:
		sayLines(2, 1500);
		return 1;
	case 115:
		sayLines(1, 1216);
		return 1;
	case 0x67:
		if (_gameState->_currentScene == 52 || _gameState->_currentScene == 53) {
			runEventScript(_mouseX, _mouseY, 2, 108, 0);
		}
		return 1;
	default:
		break;
	}
	return 0;
}

void ToonEngine::deleteItemFromInventory(int32 item) {
	for (int32 i = 0; i < _gameState->_numInventoryItems; ++i) {
		if (_gameState->_inventory[i] == item) {
			_gameState->_inventory[i] = 0;
			rearrangeInventory();
			return;
		}
	}
}

void ToonEngine::replaceItemFromInventory(int32 item, int32 newitem) {
	for (int32 i = 0; i < _gameState->_numInventoryItems; ++i) {
		if (_gameState->_inventory[i] == item) {
			_gameState->_inventory[i] = newitem;
			return;
		}
	}
}

int32 ToonEngine::pauseSceneAnimationScript(int32 animScriptId, int32 tickToWait) {
	int32 nextTicks = getTickLength() * tickToWait + getSceneAnimationScript(animScriptId)->_lastTimer;
	if (nextTicks < getOldMilli()) {
		getSceneAnimationScript(animScriptId)->_lastTimer = getOldMilli() + getTickLength() * tickToWait;
	} else {
		getSceneAnimationScript(animScriptId)->_lastTimer = nextTicks;
	}
	return nextTicks;
}

Common::Path ToonEngine::createRoomFilename(const Common::String& name) {
	Common::Path file(Common::String::format("ACT%d/%s/%s", _gameState->_currentChapter, _gameState->_locations[_gameState->_currentScene]._name, name.c_str()));
	return file;
}

void ToonEngine::createShadowLUT() {
	// here we create the redirection table that will be used to draw shadows
	// for each color of the palette we find the closest color in the palette that could be used for shadowed color.

	// In the original program, the scale factor is 0.77f
	// we will use 77 / 100 here.

	if (!_shadowLUT) {
		_shadowLUT = new uint8[256];
	}

	uint32 scaleNum = 77;
	uint32 scaleDenom = 100;

	for (int32 i = 0; i < 255; ++i) {

		// goal color
		uint32 destR = _finalPalette[i * 3 + 0] * scaleNum / scaleDenom;
		uint32 destG = _finalPalette[i * 3 + 1] * scaleNum / scaleDenom;
		uint32 destB = _finalPalette[i * 3 + 2] * scaleNum / scaleDenom;

		// search only in the "picture palette" which is in colors 1-128 and 200-255
		int32 colorDist = 0xffffff;
		int32 foundColor = 0;

		for (int32 c = 1; c < 129; ++c) {

			int32 diffR = _finalPalette[c * 3 + 0] - destR;
			int32 diffG = _finalPalette[c * 3 + 1] - destG;
			int32 diffB = _finalPalette[c * 3 + 2] - destB;

			if (colorDist > diffR * diffR + diffG * diffG + diffB * diffB) {
				colorDist = diffR * diffR + diffG * diffG + diffB * diffB;
				foundColor = c;
			}
		}

		for (int32 c = 200; c < 256; ++c) {

			int32 diffR = _finalPalette[c * 3 + 0] - destR;
			int32 diffG = _finalPalette[c * 3 + 1] - destG;
			int32 diffB = _finalPalette[c * 3 + 2] - destB;

			if (colorDist > diffR * diffR + diffG * diffG + diffB * diffB) {
				colorDist = diffR * diffR + diffG * diffG + diffB * diffB;
				foundColor = c;
			}
		}

		_shadowLUT[i] = foundColor;

	}
}

bool ToonEngine::loadToonDat() {
	Common::File in;
	Common::U32String errorMessage;
	Common::String filename = "toon.dat";
	int majVer, minVer;

	in.open(filename.c_str());

	if (!in.isOpen()) {
		const char *msg = _s("Unable to locate the '%s' engine data file.");
		errorMessage = Common::U32String::format(_(msg), filename.c_str());
		GUIErrorMessage(errorMessage);
		warning(msg, filename.c_str());
		return false;
	}

	// Read header
	char buf[4+1];
	in.read(buf, 4);
	buf[4] = '\0';

	if (strcmp(buf, "TOON")) {
		const char *msg = _s("The '%s' engine data file is corrupt.");
		errorMessage = Common::U32String::format(_(msg), filename.c_str());
		GUIErrorMessage(errorMessage);
		warning(msg, filename.c_str());
		return false;
	}

	majVer = in.readByte();
	minVer = in.readByte();

	if ((majVer != TOON_DAT_VER_MAJ) || (minVer != TOON_DAT_VER_MIN)) {
		const char *msg = _s("Incorrect version of the '%s' engine data file found. Expected %d.%d but got %d.%d.");
		errorMessage = Common::U32String::format(_(msg), filename.c_str(), TOON_DAT_VER_MAJ, TOON_DAT_VER_MIN, majVer, minVer);
		GUIErrorMessage(errorMessage);

		warning(msg, filename.c_str(), TOON_DAT_VER_MAJ, TOON_DAT_VER_MIN, majVer, minVer);
		return false;
	}

	_numVariant = in.readUint16BE();

	_locationDirNotVisited = loadTextsVariants(in);
	_locationDirVisited = loadTextsVariants(in);
	_specialInfoLine = loadTextsVariants(in);

	return true;
}

void ToonEngine::unloadToonDat() {
	unloadTextsVariants(_locationDirNotVisited);
	unloadTextsVariants(_locationDirVisited);
	unloadTextsVariants(_specialInfoLine);
}

char **ToonEngine::loadTextsVariants(Common::File &in) {
	int  len;
	char **res = nullptr;
	char *pos = nullptr;

	for (int varnt = 0; varnt < _numVariant; ++varnt) {
		int numTexts = in.readUint16BE();
		int entryLen = in.readUint16BE();
		pos = (char *)malloc(entryLen);
		if (varnt == _gameVariant) {
			res = (char **)malloc(sizeof(char *) * numTexts);
			res[0] = pos;
			in.read(res[0], entryLen);
			res[0] += DATAALIGNMENT;
		} else {
			in.read(pos, entryLen);
			free(pos);
			continue;
		}

		pos += DATAALIGNMENT;

		for (int i = 1; i < numTexts; ++i) {
			pos -= 2;

			len = READ_BE_UINT16(pos);
			pos += 2 + len;

			if (varnt == _gameVariant)
				res[i] = pos;
		}
	}

	return res;
}

void ToonEngine::unloadTextsVariants(char **texts) {
	if (!texts)
		return;

	free(*texts - DATAALIGNMENT);
	free(texts);
}

void ToonEngine::makeLineNonWalkable(int32 x, int32 y, int32 x2, int32 y2) {
	_currentMask->drawLineOnMask(x, y, x2, y2, false);
}

void ToonEngine::makeLineWalkable(int32 x, int32 y, int32 x2, int32 y2) {
	_currentMask->drawLineOnMask(x, y, x2, y2, true);
}

void ToonEngine::playRoomMusic() {
	if (_gameState->_inConversation) {
		const char* music = getSpecialConversationMusic(_gameState->_currentConversationId);
		if (music) {
			_audioManager->playMusic(_gameState->_locations[_gameState->_currentScene]._name, music);
			return;
		}
	}

	_audioManager->playMusic(_gameState->_locations[_gameState->_currentScene]._name, _gameState->_locations[_gameState->_currentScene]._music);
}

void ToonEngine::dirtyAllScreen()
{
	_dirtyRects.clear();
	_dirtyAll = true;
}

void ToonEngine::addDirtyRect( int32 left, int32 top, int32 right, int32 bottom ) {
	left = MIN<int32>(MAX<int32>(left, 0), TOON_BACKBUFFER_WIDTH);
	right = MIN<int32>(MAX<int32>(right, 0), TOON_BACKBUFFER_WIDTH);
	top = MIN<int32>(MAX<int32>(top, 0), TOON_BACKBUFFER_HEIGHT);
	bottom = MIN<int32>(MAX<int32>(bottom, 0), TOON_BACKBUFFER_HEIGHT);

	if (bottom - top <= 0 || right - left <= 0)
		return;

	Common::Rect rect(left, top, right, bottom);

	for (uint32 i = 0; i < _dirtyRects.size(); ++i) {
		if (_dirtyRects[i].contains(rect))
			return;
		if (rect.contains(_dirtyRects[i])) {
			_dirtyRects.remove_at(i);
			--i;
		}
	}

	// check also in the old rect (of the old frame)
	for (int32 i = _oldDirtyRects.size() - 1 ; i >= 0; --i) {
		if (rect.contains(_oldDirtyRects[i])) {
			_oldDirtyRects.remove_at(i);
		}
	}

	_dirtyRects.push_back(rect);
}

void ToonEngine::clearDirtyRects() {
	_oldDirtyRects = _dirtyRects;
	_dirtyRects.clear();
	_dirtyAll = false;
}


void ToonEngine::syncSoundSettings() {
	Engine::syncSoundSettings();

	_mixer->setVolumeForSoundType(_mixer->kMusicSoundType, ConfMan.getInt("music_volume"));
	_mixer->setVolumeForSoundType(_mixer->kSpeechSoundType, ConfMan.getInt("speech_volume"));
	_mixer->setVolumeForSoundType(_mixer->kSFXSoundType, ConfMan.getInt("sfx_volume"));

	if (_noMusicDriver) {
		// This affects *only* the music muting.
		_mixer->muteSoundType(_mixer->kMusicSoundType, true);
		_audioManager->muteMusic(true);
	}

	bool allSoundIsMuted = false;
	if (ConfMan.hasKey("mute")) {
		allSoundIsMuted = ConfMan.getBool("mute");
		if (!_noMusicDriver) {
			_mixer->muteSoundType(_mixer->kMusicSoundType, allSoundIsMuted);
			_audioManager->muteMusic(allSoundIsMuted);
		}
		_mixer->muteSoundType(_mixer->kSpeechSoundType, allSoundIsMuted);
		_audioManager->muteVoice(allSoundIsMuted);
		_mixer->muteSoundType(_mixer->kSFXSoundType, allSoundIsMuted);
		_audioManager->muteSfx(allSoundIsMuted);
		// movie sound type
		_mixer->muteSoundType(_mixer->kPlainSoundType, allSoundIsMuted);
	}

	if (ConfMan.hasKey("music_mute") && !allSoundIsMuted) {
		if (!_noMusicDriver) {
			_mixer->muteSoundType(_mixer->kMusicSoundType, ConfMan.getBool("music_mute"));
			_audioManager->muteMusic(ConfMan.getBool("music_mute"));
		}
	}

	if (ConfMan.hasKey("speech_mute") && !allSoundIsMuted) {
		_mixer->muteSoundType(_mixer->kSpeechSoundType, ConfMan.getBool("speech_mute"));
		_audioManager->muteVoice(ConfMan.getBool("speech_mute"));
	}

	if (ConfMan.hasKey("sfx_mute") && !allSoundIsMuted) {
		_mixer->muteSoundType(_mixer->kSFXSoundType, ConfMan.getBool("sfx_mute"));
		_audioManager->muteSfx(ConfMan.getBool("sfx_mute"));
	}

	// Adjust movie volume
	if (!allSoundIsMuted) {
		int movieVol = MAX<int>((_audioManager->isMusicMuted() ? 0 : ConfMan.getInt("music_volume")),
		                        (_audioManager->isVoiceMuted() ? 0 : ConfMan.getInt("speech_volume")));
		movieVol = MAX<int>(movieVol,(_audioManager->isSfxMuted() ? 0 : ConfMan.getInt("sfx_volume")));
		_mixer->setVolumeForSoundType(Audio::Mixer::kPlainSoundType, movieVol);
	}

	_showConversationText = ConfMan.getBool("subtitles");
	if (_showConversationText && !_isEnglishDemo) {
		setFont(ConfMan.getBool("alternative_font"));
	}

	if ((ConfMan.getInt("speech_volume") == 0 || ConfMan.getBool("speech_mute") || allSoundIsMuted)
	     && !_showConversationText) {
		ConfMan.setBool("subtitles", true);
		_showConversationText = true;
	}

	_textSpeed = ConfMan.getInt("talkspeed");

	// write-back to ini file for persistence
	ConfMan.flushToDisk();
}

void SceneAnimation::save(ToonEngine *vm, Common::WriteStream *stream) {
	stream->writeByte(_active);
	stream->writeSint32BE(_id);

	if (!_active)
		return;

	if (_animInstance) {
		stream->writeByte(1);
		_animInstance->save(stream);
	} else {
		stream->writeByte(0);
	}

	if (!_animation) {
		stream->writeByte(0);
	} else {
		stream->writeByte(strlen(_animation->_name) + 1);
		stream->write(_animation->_name, strlen(_animation->_name) + 1);
	}
}
void SceneAnimation::load(ToonEngine *vm, Common::ReadStream *stream) {

	_active = stream->readByte();
	_id = stream->readSint32BE();

	if (!_active)
		return;

	if (stream->readByte() == 1) {
		_animInstance = vm->getAnimationManager()->createNewInstance(kAnimationScene);
		_animInstance->load(stream);
		// we add them at the end of loading in reverse order
		//vm->getAnimationManager()->addInstance(_animInstance);
		_originalAnimInstance = _animInstance;
	} else {
		_animInstance = nullptr;
		_originalAnimInstance = nullptr;
	}

	// load animation if any
	char animationName[256];
	*animationName = 0;
	int8 strSize = stream->readByte();
	if (!strSize) {
		_animation = 0;
		if (_animInstance)
			_animInstance->setAnimation(0);
	} else {
		stream->read(animationName, strSize);
		animationName[strSize] = 0;

		_animation = new Animation(vm);
		_animation->loadAnimation(animationName);

		if (_animInstance) {
			_animInstance->setAnimation(_animation, false);
		}
	}
}

} // End of namespace Toon
