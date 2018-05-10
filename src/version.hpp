// Copyright (c) 2018, The Jetcash Project.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#pragma once

// defines are for Windows resource compiler
#define jetcash_VERSION_WINDOWS_COMMA 1, 18, 1, 1
#define jetcash_VERSION_STRING "1.0.0"


#ifndef RC_INVOKED  // Windows resource compiler
 
namespace jetcash {
inline const char *app_version() { return jetcash_VERSION_STRING; }
}
 
#endif
