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

#ifndef NUVIE_SOUND_CUSTOMS_FX_MANAGER_H
#define NUVIE_SOUND_CUSTOMS_FX_MANAGER_H

#include "ultima/nuvie/core/nuvie_defs.h"
#include "ultima/nuvie/conf/configuration.h"
#include "ultima/nuvie/sound/sfx_manager.h"
#include "audio/audiostream.h"
#include "audio/mixer.h"
#include "audio/decoders/wave.h"

namespace Ultima {
namespace Nuvie {

class CustomSfxManager : public SfxManager {
public:
	CustomSfxManager(const Configuration *cfg, Audio::Mixer *m);
	~CustomSfxManager() override;

	bool playSfx(SfxIdType sfx_id, uint8 volume) override;
	bool playSfxLooping(SfxIdType sfx_id, Audio::SoundHandle *handle, uint8 volume) override;

	void playSoundSample(uint16 sample_num, Audio::SoundHandle *looping_handle, uint8 volume);

private:
	bool loadSfxMapFile(const Common::Path &cfg_filename, Common::HashMap<uint16, uint16> *m);

private:
	Common::Path custom_filepath;

	Common::HashMap<uint16, uint16> *sfx_map;
};

} // End of namespace Nuvie
} // End of namespace Ultima

#endif
