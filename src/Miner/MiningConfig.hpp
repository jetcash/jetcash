// Copyright (c) 2018, The Jetcash Project.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#pragma once

#include <cstdint>
#include <string>
#include "common/CommandLine.hpp"

namespace jetcash {

struct MiningConfig {
	explicit MiningConfig(common::CommandLine &cmd);

	std::string mining_address;
	std::string jetcashd_ip;
	uint16_t jetcashd_port = 0;
	size_t thread_count     = 0;
	//	size_t scan_period; // We are using longpoll now
	//	uint8_t log_level;
	size_t blocks_limit = 0;  // Mine specified number of blocks, then exit, 0 == indefinetely
	                          //	uint64_t first_block_timestamp;
	                          //	int64_t block_timestamp_interval;
	                          //	bool help;
};

}  // namespace jetcash
