// Copyright (c) 2018, The Jetcash Project.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#pragma once

#include <cstdint>
#include <vector>

#include "CryptoNote.hpp"

namespace jetcash {

bool check_hash(const crypto::Hash &hash, Difficulty difficulty);
}
