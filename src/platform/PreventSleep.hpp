// Copyright (c) 2018, The Jetcash Project.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#pragma once

namespace platform {

class PreventSleep {
public:
	explicit PreventSleep(const char *reason);  // some OSes will show this string to user
	~PreventSleep();
};
}
