/*
 * Copyright © 2018-2023 Synthstrom Audible Limited
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

#include "gui/context_menu/context_menu.h"

namespace deluge::gui::context_menu::sample_browser {
class Synth final : public ContextMenu {
public:
	Synth() = default;

	Sized<char const**> getOptions() override;
	bool isCurrentOptionAvailable() override;
	bool canSeeViewUnderneath() override;

	bool acceptCurrentOption() override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;

	char const* getTitle() override;
};

extern Synth synth;
} // namespace deluge::gui::context_menu::sample_browser
