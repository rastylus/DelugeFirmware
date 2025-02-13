/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include "gui/ui/keyboard/layout.h"

namespace deluge::gui::ui::keyboard::layout {

constexpr int32_t kMinNornsRowInterval = 1;
constexpr int32_t kMaxNornsRowInterval = 16;

class KeyboardLayoutNorns : public KeyboardLayout {
public:
	KeyboardLayoutNorns() {}
	~KeyboardLayoutNorns() override {}

	void evaluatePads(PressedPad presses[kMaxNumKeyboardPadPresses]) override;
	void handleVerticalEncoder(int32_t offset) override;
	void handleHorizontalEncoder(int32_t offset, bool shiftEnabled) override;
	void precalculate() override;

	void renderPads(uint8_t image[][kDisplayWidth + kSideBarWidth][3]) override;

	char const* name() override { return "Norns"; }
	bool supportsInstrument() override { return true; }
	bool supportsKit() override { return false; }

private:
	inline uint8_t noteFromCoords(int32_t x, int32_t y) { return x + y * kDisplayWidth; }
};

}; // namespace deluge::gui::ui::keyboard::layout
