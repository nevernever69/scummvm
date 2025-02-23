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

#include "kyra/engine/util.h"
#include "kyra/resource/resource.h"
#include "kyra/resource/resource_intern.h"

#include "common/macresman.h"
#include "common/compression/stuffit.h"
#include "common/concatstream.h"
#include "common/config-manager.h"
#include "common/fs.h"
#include "common/substream.h"

namespace Kyra {

Common::Archive *Resource::loadKyra1MacInstaller() {
	Common::Path kyraInstaller = Util::findMacResourceFile("Install Legend of Kyrandia");

	if (!kyraInstaller.empty()) {
		Common::Archive *archive = loadStuffItArchive(kyraInstaller, "Install Legend of Kyrandia");
		if (!archive)
			error("Failed to load Legend of Kyrandia installer file");
		return archive;
	}

	kyraInstaller = Util::findMacResourceFile("Legend of Kyrandia", " Installer");

	if (!kyraInstaller.empty()) {
		Common::Array<Common::SharedPtr<Common::SeekableReadStream>> parts;
		for (int i = 1; i <= 5; i++) {
			Common::Path partName = i == 1 ? kyraInstaller : kyraInstaller.append(Common::String::format(".%d", i));
			Common::SeekableReadStream *stream = Common::MacResManager::openFileOrDataFork(partName);
			if (!stream)
				error("Failed to load Legend of Kyrandia installer file part %s", partName.toString().c_str());
			if (stream->size() <= 100)
				error("Legend of Kyrandia installer file part %s is too short", partName.toString().c_str());
			parts.push_back(Common::SharedPtr<Common::SeekableReadStream>(new Common::SeekableSubReadStream(stream, 100, stream->size(), DisposeAfterUse::YES)));
		}
		return loadStuffItArchive(new Common::ConcatReadStream(parts), "Install Legend of Kyrandia", "Legend of Kyrandia(TM) Installer.*");
	}

	return nullptr;
}

Resource::Resource(KyraEngine_v1 *vm) : _archiveCache(), _files(), _archiveFiles(), _protectedFiles(), _loaders(), _vm(vm), _bigEndianPlatForm(vm->gameFlags().platform == Common::kPlatformAmiga || vm->gameFlags().platform == Common::kPlatformSegaCD) {
	initializeLoaders();

	if (_vm->game() == GI_KYRA1 && _vm->gameFlags().platform == Common::Platform::kPlatformMacintosh)
		SearchMan.addSubDirectoryMatching(Common::FSNode(ConfMan.getPath("path")), "runtime");

	// Initialize directories for playing from CD or with original
	// directory structure
	if (_vm->game() == GI_KYRA3)
		SearchMan.addSubDirectoryMatching(Common::FSNode(ConfMan.getPath("path")), "malcolm");

	if (_vm->game() == GI_LOL)
		SearchMan.addSubDirectoryMatching(Common::FSNode(ConfMan.getPath("path")), "data", 0, 2);

	_files.add("global_search", &Common::SearchManager::instance(), 3, false);
	// compressed installer archives are added at level '2',
	// but that's done in Resource::reset not here
	_files.add("protected", &_protectedFiles, 1, false);
	_files.add("archives", &_archiveFiles, 0, false);
}

Resource::~Resource() {
	_loaders.clear();

	for (ArchiveMap::iterator i = _archiveCache.begin(); i != _archiveCache.end(); ++i)
		delete i->_value;
	_archiveCache.clear();
}

bool Resource::reset() {
	unloadAllPakFiles();

	Common::FSNode dir(ConfMan.getPath("path"));

	if (!dir.exists() || !dir.isDirectory())
		error("invalid game path '%s'", dir.getPath().toString(Common::Path::kNativeSeparator).c_str());

	if (_vm->game() == GI_KYRA1 && _vm->gameFlags().platform == Common::kPlatformMacintosh && _vm->gameFlags().useInstallerPackage) {
		Common::Archive *archive = loadKyra1MacInstaller();
		if (!archive)
			error("Could not find Legend of Kyrandia installer file");

		_files.add("installer", archive, 0, false);

		Common::ArchiveMemberList members;
		archive->listMatchingMembers(members, "*.PAK");
		for (Common::ArchiveMemberList::const_iterator it = members.begin(); it != members.end(); ++it) {
			Common::String name = (*it)->getName();
			Common::Archive *pak = loadArchive(name, *it);
			_files.add(name, pak, 0, false);
		}
	} else if (_vm->game() == GI_KYRA1 || _vm->game() == GI_EOB1) {
		// We only need kyra.dat for the demo.
		if (_vm->gameFlags().isDemo && !_vm->gameFlags().isTalkie)
			return true;

		if (!_vm->gameFlags().isDemo && _vm->gameFlags().isTalkie) {
			// List of files in the talkie version, which can never be unload.
			static const char *const list[] = {
				"ADL.PAK", "CHAPTER1.VRM", "COL.PAK", "FINALE.PAK", "INTRO1.PAK", "INTRO2.PAK",
				"INTRO3.PAK", "INTRO4.PAK", "MISC.PAK", "SND.PAK", "STARTUP.PAK", "XMI.PAK",
				"CAVE.APK", "DRAGON1.APK", "DRAGON2.APK", "LAGOON.APK", nullptr
			};

			loadProtectedFiles(list);
		} else {
			// We only search in the game path to avoid any invalid PAK or
			// APK files from being picked up. This might happen, for example,
			// when the user has an Android package file in the CWD.
			Common::FSDirectory gameDir(dir);
			Common::ArchiveMemberList files;
			Common::ScopedPtr<Common::FSDirectory> gameDirRuntime;

			gameDir.listMatchingMembers(files, "*.PAK");
			gameDir.listMatchingMembers(files, "*.APK");

			if (_vm->gameFlags().platform == Common::Platform::kPlatformMacintosh && gameDir.getSubDirectory("runtime") != nullptr) {
				gameDirRuntime.reset(gameDir.getSubDirectory("runtime"));
				gameDirRuntime->listMatchingMembers(files, "*.PAK");
				gameDirRuntime->listMatchingMembers(files, "*.APK");
			}

			for (Common::ArchiveMemberList::const_iterator i = files.begin(); i != files.end(); ++i) {
				Common::String name = (*i)->getName();
				name.toUppercase();

				// No PAK file
				if (name == "TWMUSIC.PAK" || name == "EYE.PAK")
					continue;

				// We need to only load the script archive for the language the user specified
				if (name == ((_vm->gameFlags().lang == Common::EN_ANY) ? "JMC.PAK" : "EMC.PAK"))
					continue;

				Common::Archive *archive = loadArchive((*i)->getName(), *i);

				if (archive) {
					// Hack for the Spanish version of EOB1. It has an invalid item.dat file in the
					// game directory that needs to have a lower priority than the one in EOBDATA6.PAK.
					bool highPrio = (_vm->game() == GI_EOB1 && _vm->gameFlags().lang == Common::ES_ESP && archive->hasFile("ITEM.DAT"));
					_files.add(name, archive, highPrio ? 4 : 0, false);
				}
				else {
					error("Couldn't load PAK file '%s'", name.c_str());
				}
			}
		}
	} else if (_vm->game() == GI_KYRA2) {
		if (_vm->gameFlags().useInstallerPackage)
			_files.add("installer", loadInstallerArchive("WESTWOOD", "%03d", 6), 2, false);

		// mouse pointer, fonts, etc. required for initialization
		if (_vm->gameFlags().isDemo && !_vm->gameFlags().isTalkie) {
			loadPakFile("GENERAL.PAK");
		} else {
			loadPakFile("INTROGEN.PAK");
			loadPakFile("OTHER.PAK");
		}
	} else if (_vm->game() == GI_KYRA3) {
		loadPakFile("WESTWOOD.001");

		if (!loadFileList("FILEDATA.FDT"))
			error("Couldn't load file: 'FILEDATA.FDT'");
	} else if (_vm->game() == GI_LOL) {
		if (_vm->gameFlags().useInstallerPackage)
			_files.add("installer", loadInstallerArchive("WESTWOOD", "%d", 0), 2, false);

		if (!_vm->gameFlags().isTalkie && !_vm->gameFlags().isDemo) {
			static const char *const list[] = {
				"GENERAL.PAK", nullptr
			};

			loadProtectedFiles(list);
		}
	} else if (_vm->game() != GI_EOB2) {
		error("Unknown game id: %d", _vm->game());
		return false;   // for compilers that don't support NORETURN
	}

	return true;
}

bool Resource::loadPakFile(const Common::Path &filename) {
	Common::Path filenameFixed(filename);
	filenameFixed.toUppercase();

	Common::ArchiveMemberPtr file = _files.getMember(filenameFixed);
	if (!file)
		return false;

	return loadPakFile(filenameFixed.toString('/'), file);
}

bool Resource::loadPakFile(const Common::String &name, Common::ArchiveMemberPtr file) {
	Common::String nameFixed(name);
	nameFixed.toUppercase();

	if (_archiveFiles.hasArchive(nameFixed) || _protectedFiles.hasArchive(nameFixed))
		return true;

	Common::Archive *archive = loadArchive(nameFixed, file);
	if (!archive)
		return false;

	_archiveFiles.add(nameFixed, archive, 0, false);

	return true;
}

bool Resource::loadFileList(const Common::Path &filedata) {
	Common::SeekableReadStream *f = createReadStream(filedata);

	if (!f)
		return false;

	uint32 filenameOffset = 0;
	while ((filenameOffset = f->readUint32LE()) != 0) {
		uint32 offset = f->pos();
		f->seek(filenameOffset, SEEK_SET);

		uint8 buffer[13];
		f->read(buffer, sizeof(buffer) - 1);
		buffer[12] = 0;
		f->seek(offset + 16, SEEK_SET);

		Common::String filename = Common::String((char *)buffer);
		filename.toUppercase();

		if (filename.hasSuffix(".PAK")) {
			Common::Path path(filename);
			if (!exists(path) && _vm->gameFlags().isDemo) {
				// the demo version supplied with Kyra3 does not
				// contain all pak files listed in filedata.fdt
				// so we don't do anything here if they are non
				// existent.
			} else if (!loadPakFile(path)) {
				delete f;
				error("couldn't load file '%s'", filename.c_str());
				return false;   // for compilers that don't support NORETURN
			}
		}
	}

	delete f;
	return true;
}

bool Resource::loadFileList(const char *const *filelist, uint32 numFiles) {
	if (!filelist)
		return false;

	while (numFiles--) {
		if (!loadPakFile(filelist[numFiles])) {
			error("couldn't load file '%s'", filelist[numFiles]);
			return false;   // for compilers that don't support NORETURN
		}
	}

	return true;
}

bool Resource::loadProtectedFiles(const char *const *list) {
	for (uint i = 0; list[i]; ++i) {
		Common::ArchiveMemberPtr file = _files.getMember(list[i]);
		if (!file)
			error("Couldn't find PAK file '%s'", list[i]);

		Common::Archive *archive = loadArchive(list[i], file);
		if (archive)
			_protectedFiles.add(list[i], archive, 0, false);
		else
			error("Couldn't load PAK file '%s'", list[i]);
	}

	return true;
}

void Resource::unloadPakFile(const Common::String &name, bool remFromCache) {
	Common::String nameFixed(name);
	nameFixed.toUppercase();

	// We do not remove files from '_protectedFiles' here, since
	// those are protected against unloading.
	if (_archiveFiles.hasArchive(nameFixed)) {
		_archiveFiles.remove(nameFixed);
		if (remFromCache) {
			ArchiveMap::iterator iter = _archiveCache.find(nameFixed);
			if (iter != _archiveCache.end()) {
				delete iter->_value;
				_archiveCache.erase(nameFixed);
			}
		}
	}
}

bool Resource::isInPakList(const Common::String &name) {
	Common::String nameFixed(name);
	nameFixed.toUppercase();
	return (_archiveFiles.hasArchive(nameFixed) || _protectedFiles.hasArchive(nameFixed));
}

bool Resource::isInCacheList(const Common::String &name) {
	Common::String nameFixed(name);
	nameFixed.toUppercase();
	return (_archiveCache.find(nameFixed) != _archiveCache.end());
}

void Resource::unloadAllPakFiles() {
	_archiveFiles.clear();
	_protectedFiles.clear();
}

void Resource::listFiles(const Common::Path &pattern, Common::ArchiveMemberList &list) {
	_files.listMatchingMembers(list, pattern);
}

uint8 *Resource::fileData(const Common::Path &file, uint32 *size) {
	Common::SeekableReadStream *stream = createReadStream(file);
	if (!stream)
		return nullptr;

	uint32 bufferSize = stream->size();
	uint8 *buffer = new uint8[bufferSize];
	assert(buffer);
	if (size)
		*size = bufferSize;
	stream->read(buffer, bufferSize);
	delete stream;
	return buffer;
}

bool Resource::exists(const Common::Path &file, bool errorOutOnFail) {
	if (_files.hasFile(file))
		return true;
	else if (errorOutOnFail)
		error("File '%s' can't be found", file.toString().c_str());
	return false;
}

uint32 Resource::getFileSize(const Common::Path &file) {
	Common::SeekableReadStream *stream = createReadStream(file);
	if (!stream)
		return 0;

	uint32 size = stream->size();
	delete stream;
	return size;
}

bool Resource::loadFileToBuf(const Common::Path &file, void *buf, uint32 maxSize) {
	Common::SeekableReadStream *stream = createReadStream(file);
	if (!stream)
		return false;

	memset(buf, 0, maxSize);
	stream->read(buf, ((int32)maxSize <= stream->size()) ? maxSize : stream->size());
	delete stream;
	return true;
}

Common::Archive *Resource::getCachedArchive(const Common::String &file) const {
	ArchiveMap::iterator a = _archiveCache.find(file);
	return a != _archiveCache.end() ? a->_value : 0;
}

Common::SeekableReadStream *Resource::createReadStream(const Common::Path &file) {
	return _files.createReadStreamForMember(file);
}

Common::SeekableReadStreamEndian *Resource::createEndianAwareReadStream(const Common::Path &file, int endianness) {
	Common::SeekableReadStream *stream = _files.createReadStreamForMember(file);
	return stream ? new Common::SeekableReadStreamEndianWrapper(stream, (endianness == kForceBE) ? true : (endianness == kForceLE ? false : _bigEndianPlatForm), DisposeAfterUse::YES) : nullptr;
}

Common::Archive *Resource::loadArchive(const Common::String &name, Common::ArchiveMemberPtr member) {
	ArchiveMap::iterator cachedArchive = _archiveCache.find(name);
	if (cachedArchive != _archiveCache.end())
		return cachedArchive->_value;

	Common::SeekableReadStream *stream = member->createReadStream();

	if (!stream)
		return nullptr;

	Common::Archive *archive = nullptr;
	for (LoaderList::const_iterator i = _loaders.begin(); i != _loaders.end(); ++i) {
		if ((*i)->checkFilename(name)) {
			if ((*i)->isLoadable(name, *stream)) {
				stream->seek(0, SEEK_SET);
				archive = (*i)->load(member, *stream);
				break;
			} else {
				stream->seek(0, SEEK_SET);
			}
		}
	}

	delete stream;

	if (!archive)
		return nullptr;

	_archiveCache[name] = archive;
	return archive;
}

Common::Archive *Resource::loadInstallerArchive(const Common::Path &file, const Common::String &ext, const uint8 offset) {
	Common::String name(file.toString('/'));
	ArchiveMap::iterator cachedArchive = _archiveCache.find(name);
	if (cachedArchive != _archiveCache.end())
		return cachedArchive->_value;

	Common::Archive *archive = InstallerLoader::load(this, file, ext, offset);
	if (!archive)
		return nullptr;

	_archiveCache[name] = archive;
	return archive;
}

Common::Archive *Resource::loadStuffItArchive(const Common::Path &file, const Common::String &canonicalName) {
	ArchiveMap::iterator cachedArchive = _archiveCache.find(canonicalName);
	if (cachedArchive != _archiveCache.end())
		return cachedArchive->_value;

	Common::Archive *archive = StuffItLoader::load(this, file);
	if (!archive)
		return nullptr;

	_archiveCache[canonicalName] = archive;
	return archive;
}

Common::Archive *Resource::loadStuffItArchive(Common::SeekableReadStream *stream, const Common::String &canonicalName, const Common::String &debugName) {
	ArchiveMap::iterator cachedArchive = _archiveCache.find(canonicalName);
	if (cachedArchive != _archiveCache.end()) {
		delete stream;
		return cachedArchive->_value;
	}

	Common::Archive *archive = StuffItLoader::load(this, stream, debugName);
	if (!archive)
		return nullptr;

	_archiveCache[canonicalName] = archive;
	return archive;
}

#pragma mark -

void Resource::initializeLoaders() {
	_loaders.push_back(LoaderList::value_type(new ResLoaderPak()));
	_loaders.push_back(LoaderList::value_type(new ResLoaderInsMalcolm()));
	_loaders.push_back(LoaderList::value_type(new ResLoaderTlk()));
}

} // End of namespace Kyra
