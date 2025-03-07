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

#include "m4/burger/console.h"
#include "m4/burger/vars.h"
#include "m4/burger/burger.h"

namespace M4 {
namespace Burger {

Console::Console() : M4::Console() {
	registerCmd("test", WRAP_METHOD(Console, cmdTest));
}

bool Console::cmdTest(int argc, const char **argv) {
	int tests = _G(flags)[kFirstTestPassed] ? 1 : 0 +
		_G(flags)[kSecondTestPassed] ? 1 : 0 +
		_G(flags)[kThirdTestPassed] ? 1 : 0 +
		_G(flags)[kFourthTestPassed] ? 1 : 0 +
		_G(flags)[kFifthTestPassed] ? 1 : 0;

	debugPrintf("Tests passed = %d\n", tests);
	return true;
}


} // End of namespace Burger
} // End of namespace M4
