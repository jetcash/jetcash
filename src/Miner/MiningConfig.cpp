// Copyright (c) 2018, The Jetcash Project.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#include "MiningConfig.hpp"
#include "common/CommandLine.hpp"
#include "common/Ipv4Address.hpp"

#include <iostream>
#include <thread>

#include "CryptoNoteConfig.hpp"
#include "logging/ILogger.hpp"

using namespace jetcash;

MiningConfig::MiningConfig(common::CommandLine &cmd)
    : jetcashd_ip("127.0.0.1"), jetcashd_port(RPC_DEFAULT_PORT), thread_count(std::thread::hardware_concurrency()) {
	if (const char *pa = cmd.get("--address"))
		mining_address = pa;
	if (const char *pa = cmd.get("--jetcashd-address")) {
		if (!common::parse_ip_address_and_port(pa, jetcashd_ip, jetcashd_port))
			throw std::runtime_error("Wrong address format " + std::string(pa) + ", should be ip:port");
	}
	if (const char *pa = cmd.get("--daemon-address", "Use --jetcashd-address instead")) {
		if (!common::parse_ip_address_and_port(pa, jetcashd_ip, jetcashd_port))
			throw std::runtime_error("Wrong address format " + std::string(pa) + ", should be ip:port");
	}
	if (const char *pa = cmd.get("--daemon-host", "Use --jetcashd-address instead"))
		jetcashd_ip = pa;
	if (const char *pa = cmd.get("--daemon-rpc-port", "Use --jetcashd-address instead"))
		jetcashd_port = boost::lexical_cast<uint16_t>(pa);
	if (const char *pa = cmd.get("--threads"))
		thread_count = boost::lexical_cast<size_t>(pa);
	if (const char *pa = cmd.get("--limit"))
		blocks_limit = boost::lexical_cast<size_t>(pa);
}
