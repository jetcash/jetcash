// Copyright (c) 2018, The Jetcash Project.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#include <iostream>
#include "Config.hpp"
#include "Node.hpp"
#include "seria/BinaryInputStream.hpp"
#include "seria/BinaryOutputStream.hpp"

using namespace jetcash;

static const bool multicore = true;

Node::DownloaderV11::DownloaderV11(Node *node, BlockChainState &block_chain)
    : m_node(node)
    , m_block_chain(block_chain)
    , m_chain_timer(std::bind(&DownloaderV11::on_chain_timer, this))
    , m_download_timer(std::bind(&DownloaderV11::on_download_timer, this))
    , log_request_timestamp(std::chrono::steady_clock::now())
    , log_response_timestamp(std::chrono::steady_clock::now()) {
	if (multicore) {
		auto th_count = std::max<size_t>(2, std::thread::hardware_concurrency() / 2);
		// we use more energy but have the same speed when using hyperthreading
		std::cout << "Starting multicore POW checker using " << th_count << "/" << std::thread::hardware_concurrency()
		          << " cpus" << std::endl;
		for (size_t i = 0; i != th_count; ++i)
			threads.emplace_back(&DownloaderV11::thread_run, this);
		main_loop = platform::EventLoop::current();
	}
	m_download_timer.once(SYNC_TIMEOUT / 8);  // just several ticks per SYNC_TIMEOUT
}

Node::DownloaderV11::~DownloaderV11() {
	{
		std::unique_lock<std::mutex> lock(mu);
		quit = true;
		have_work.notify_all();
	}
	for (auto &&th : threads)
		th.join();
}

void Node::DownloaderV11::add_work(std::tuple<Hash, bool, RawBlock> &&wo) {
	std::unique_lock<std::mutex> lock(mu);
	work.push_back(std::move(wo));
	have_work.notify_all();
}

void Node::DownloaderV11::thread_run() {
	crypto::CryptoNightContext hash_crypto_context;
	while (true) {
		std::tuple<Hash, bool, RawBlock> wo;
		{
			std::unique_lock<std::mutex> lock(mu);
			if (quit)
				return;
			if (work.empty()) {
				have_work.wait(lock);
				continue;
			}
			wo = std::move(work.front());
			work.pop_front();
		}
		PreparedBlock result(std::move(std::get<2>(wo)), std::get<1>(wo) ? &hash_crypto_context : nullptr);
		{
			std::unique_lock<std::mutex> lock(mu);
			prepared_blocks[std::get<0>(wo)] = std::move(result);
			main_loop->wake();  // so we start processing on_idle
		}
	}
}

uint32_t Node::DownloaderV11::get_known_block_count(uint32_t my) const {
	for (auto &&gc : m_good_clients)
		my = std::max(my, gc.first->get_last_received_sync_data().current_height);
	return my;
}

void Node::DownloaderV11::on_connect(P2PClientJetcash *who) {
	if (who->is_incoming())  // Never sync from incoming
		return;
	m_node->m_log(logging::TRACE) << "DownloaderV11::on_connect " << who->get_address() << std::endl;
	if (who->get_version() == 1) {
		m_good_clients.insert(std::make_pair(who, 0));
		if (who->get_last_received_sync_data().top_id == m_block_chain.get_tip_bid()) {
			m_node->m_log(logging::TRACE) << "DownloaderV11::on_connect sync_transactions to " << who->get_address() << std::endl;
			who->get_node()->sync_transactions(who);
			// If we at same height, sync tx now, otherwise will sync after we reach same height
		}
		advance_download(Hash{});
	}
}

void Node::DownloaderV11::on_disconnect(P2PClientJetcash *who) {
	if (who->is_incoming())
		return;
	m_node->m_log(logging::TRACE) << "DownloaderV11::on_disconnect " << who->get_address() << std::endl;
	if (total_downloading_blocks < m_good_clients[who])
		throw std::logic_error("total_downloading_blocks mismatch in disconnect");
	total_downloading_blocks -= m_good_clients[who];
	m_good_clients.erase(who);
	for (auto lit = m_who_downloaded_block.begin(); lit != m_who_downloaded_block.end();)
		if (*lit == who)
			lit = m_who_downloaded_block.erase(lit);
		else
			++lit;
	for (auto &&dc : m_download_chain) {
		if (dc.status != DownloadCell::DOWNLOADING || dc.downloading_client != who)
			continue;
		dc.downloading_client = nullptr;
	}
	if (m_chain_client && m_chain_client == who) {
 		m_chain_timer.cancel();
 		m_chain_client = nullptr;
		m_node->m_log(logging::TRACE) << "DownloaderV11::on_disconnect m_chain_client reset to 0" << std::endl;
 	}
	advance_download(Hash{});
}

void Node::DownloaderV11::on_chain_timer() {
	if (m_chain_client) {
		m_node->m_log(logging::TRACE) << "DownloaderV11::on_chain_timer" << std::endl;
		m_chain_client->disconnect(std::string());
	}
}

void Node::DownloaderV11::on_msg_notify_request_chain(P2PClientJetcash *who,
    const NOTIFY_RESPONSE_CHAIN_ENTRY::request &req) {
	if (m_chain_client != who || !m_chain.empty())
		return;  // TODO - who just sent us chain we did not ask, ban
	std::cout << "Received chain from " << who->get_address() << " start_height=" << req.start_height
	          << " length=" << req.m_block_ids.size() << std::endl;
	m_node->m_log(logging::TRACE) << "DownloaderV11::on_msg_notify_request_chain from " << who->get_address() << " start_height=" << req.start_height
	          << " length=" << req.m_block_ids.size() << std::endl;
	m_chain_start_height = req.start_height;
	chain_source         = m_chain_client->get_address();
	m_chain.assign(req.m_block_ids.begin(), req.m_block_ids.end());
	Hash last_downloaded_block = m_chain.empty() ? Hash{} : m_chain.back();
	std::set<Hash> downloading_bids;
	for (auto &&dc : m_download_chain)
		downloading_bids.insert(dc.bid);
	while (!m_chain.empty() &&
	       (m_node->m_block_chain.has_block(m_chain.front()) || downloading_bids.count(m_chain.front()) != 0)) {
		m_chain.pop_front();
		m_chain_start_height += 1;
	}  // We stop removing as soon as we find new block, because wrong order might
	// prevent us from applying blocks
	if (m_chain.empty() && req.m_block_ids.size() > 1 && last_downloaded_block != Hash{} &&
	    m_chain_client->get_last_received_sync_data().current_height >
	        m_block_chain.get_tip_height() + m_download_chain.size()) {  // No new blocks arrived
		NOTIFY_REQUEST_CHAIN::request msg;
		msg.block_ids.push_back(last_downloaded_block);
		msg.block_ids.push_back(m_node->m_block_chain.get_genesis_bid());

		std::cout << "Requesting more chain from " << m_chain_client->get_address()
		          << " remote height=" << m_chain_client->get_last_received_sync_data().current_height
		          << " our height=" << m_block_chain.get_tip_height() << " jumping from "
		          << common::pod_to_hex(last_downloaded_block) << std::endl;
		m_node->m_log(logging::TRACE) << "DownloaderV11::on_msg_notify_request_chain requesting more chain from " << m_chain_client->get_address()
		          << " remote height=" << m_chain_client->get_last_received_sync_data().current_height
		          << " our height=" << m_block_chain.get_tip_height() << " jumping from "
		          << common::pod_to_hex(last_downloaded_block) << std::endl;
		BinaryArray raw_msg = LevinProtocol::send_message(NOTIFY_REQUEST_CHAIN::ID, LevinProtocol::encode(msg), false);
		m_chain_client->send(std::move(raw_msg));
		m_chain_timer.once(SYNC_TIMEOUT);
		return;
	}
	if (req.m_block_ids.size() != m_chain.size() + 1){
		std::cout << "    truncated chain length=" << m_chain.size() << std::endl;
		m_node->m_log(logging::TRACE) << "DownloaderV11::on_msg_notify_request_chain truncated chain length=" << m_chain.size() << std::endl;
	}
	m_chain_client = nullptr;
	m_chain_timer.cancel();
	m_node->m_log(logging::TRACE) << "DownloaderV11::on_msg_notify_request_chain m_chain_client reset to 0" << std::endl;
	advance_download(Hash{});
}

static const size_t GOOD_LAG = 5;  // lagging by 5 blocks is ok for us

void Node::DownloaderV11::advance_chain() {
	if (m_chain_client || !m_chain.empty())
		return;
	std::vector<P2PClientJetcash *> lagging_clients;
	std::vector<P2PClientJetcash *> sorted_clients;
	const auto now = m_node->m_p2p.get_local_time();
	for (auto &&who : m_good_clients) {
		if (who.first->get_last_received_sync_data().current_height + GOOD_LAG < m_node->m_block_chain.get_tip_height())
			lagging_clients.push_back(who.first);
		else
			sorted_clients.push_back(who.first);
	}
	std::sort(sorted_clients.begin(), sorted_clients.end(), [](P2PClientJetcash *a, P2PClientJetcash *b) -> bool {
		return a->get_last_received_sync_data().current_height < b->get_last_received_sync_data().current_height;
	});
	if (!lagging_clients.empty()) {
		auto who = lagging_clients.front();
		m_node->m_peer_db.delay_connection_attempt(who->get_address(), now);
		std::cout << "Disconnecting lagging client " << who->get_address() << std::endl;
		m_node->m_log(logging::TRACE) << "DownloaderV11::advance_chain disconnecting lagging client " << who->get_address() << std::endl;
		who->disconnect(std::string());  // Will recursively call advance_chain again
		return;
	}
	if (sorted_clients.empty() ||
	    sorted_clients.back()->get_last_received_sync_data().current_height <=
	        m_block_chain.get_tip_height() + m_download_chain.size()) {
		return;  // If m_download_chain is not empty, it will become empty soon and
		         // we will ask for chain again
	}
	m_chain_client = sorted_clients.back();
	NOTIFY_REQUEST_CHAIN::request msg;
	msg.block_ids = m_block_chain.get_sparse_chain();

	std::cout << "Requesting chain from " << m_chain_client->get_address()
	          << " remote height=" << m_chain_client->get_last_received_sync_data().current_height
	          << " our height=" << m_block_chain.get_tip_height() << std::endl;
	m_node->m_log(logging::TRACE) << "DownloaderV11::advance_chain Requesting chain from " << m_chain_client->get_address()
	          << " remote height=" << m_chain_client->get_last_received_sync_data().current_height
	          << " our height=" << m_block_chain.get_tip_height() << std::endl;
	BinaryArray raw_msg = LevinProtocol::send_message(NOTIFY_REQUEST_CHAIN::ID, LevinProtocol::encode(msg), false);
	m_chain_client->send(std::move(raw_msg));
	m_chain_timer.once(SYNC_TIMEOUT);
}

void Node::DownloaderV11::on_msg_timed_sync(const CORE_SYNC_DATA &payload_data) { advance_download(Hash{}); }

void Node::DownloaderV11::on_msg_notify_request_objects(P2PClientJetcash *who,
    const NOTIFY_RESPONSE_GET_OBJECTS::request &req) {
	for (auto &&rb : req.blocks) {
		Hash bid;
		try {
			BlockTemplate bheader;
			seria::from_binary(bheader, rb.block);
			bid = jetcash::get_block_hash(bheader);
		} catch (const std::exception &ex) {
			std::cout << "Exception " << ex.what() << " while parsing returned block, banning " << who->get_address()
			          << std::endl;
			who->disconnect(std::string());
			break;
		}
		bool cell_found = false;
		for (auto &&dc : m_download_chain) {
			if (dc.status != DownloadCell::DOWNLOADING || dc.downloading_client != who || dc.bid != bid)
				continue;  // downloaded or downloading
			dc.status             = DownloadCell::DOWNLOADED;
			dc.downloading_client = nullptr;
			dc.rb.block           = rb.block;         // TODO - std::move
			dc.rb.transactions    = rb.transactions;  // TODO - std::move
			auto git              = m_good_clients.find(who);
			if (git == m_good_clients.end() || git->second == 0 || total_downloading_blocks == 0)
				throw std::logic_error("DownloadCell reference to good client not found");
			git->second -= 1;
			total_downloading_blocks -= 1;
			m_who_downloaded_block.push_back(who);
			auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - log_response_timestamp).count() > 1000) {
				log_response_timestamp = now;
				std::cout << "Received block with height=" << dc.expected_height
				          << " (queue=" << total_downloading_blocks << ") from " << who->get_address() << std::endl;
			}
			m_node->m_log(logging::TRACE) << "DownloaderV11::on_msg_notify_request_objects received block with height=" << dc.expected_height << " hash=" << common::pod_to_hex(dc.bid)
				          << " (queue=" << total_downloading_blocks << ") from " << who->get_address() << std::endl;
			cell_found = true;
			if (multicore) {
				dc.status = DownloadCell::PREPARING;
				add_work(std::tuple<Hash, bool, RawBlock>(dc.bid,
				    !m_node->m_block_chain.get_currency().is_in_checkpoint_zone(dc.expected_height), std::move(dc.rb)));
			} else {
				dc.pb     = PreparedBlock(std::move(dc.rb), nullptr);
				dc.status = DownloadCell::PREPARED;
			}
			break;
		}
		if (!cell_found) {
			std::cout << "Received stray block from " << who->get_address() << " banning..." << std::endl;
			m_node->m_log(logging::TRACE) << "DownloaderV11::on_msg_notify_request_objects received stray block from " << who->get_address() << " banning..." << std::endl;
			who->disconnect(std::string());
			break;
		}
	}
	advance_download(Hash{});
}

bool Node::DownloaderV11::on_idle() {
	int added_counter = 0;
	if (multicore) {
		std::unique_lock<std::mutex> lock(mu);
		for (auto &&pb : prepared_blocks) {
			for (auto &&dc : m_download_chain)
				if (dc.status == DownloadCell::PREPARING && dc.bid == pb.first) {
					dc.pb     = std::move(pb.second);
					dc.status = DownloadCell::PREPARED;
					break;
				}
		}
		prepared_blocks.clear();
	}
	auto idea_start = std::chrono::high_resolution_clock::now();
	while (!m_download_chain.empty() && m_download_chain.front().status == DownloadCell::PREPARED) {
		DownloadCell dc = std::move(m_download_chain.front());
		m_download_chain.pop_front();
		api::BlockHeader info;
		if (m_block_chain.add_block(dc.pb, info) == BroadcastAction::BAN) {
			std::cout << "DownloadCell BAN height=" << dc.expected_height << " wb=" << common::pod_to_hex(dc.bid)
			          << std::endl;
			m_node->m_log(logging::TRACE) << "DownloaderV11::on_idle DownloadCell BAN height=" << dc.expected_height << " wb=" << common::pod_to_hex(dc.bid)
			          << std::endl;
			// TODO - ban client who gave us chain
			//			continue;
		}
		added_counter += 1;
		auto idea_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::high_resolution_clock::now() - idea_start);
		if (idea_ms.count() > 100)
			break;
	}
	if (added_counter) {
		m_node->advance_long_poll();
		advance_download(Hash{});
		if (m_download_chain.empty())
			for (auto &&who : m_good_clients) {
				if (who.first->get_last_received_sync_data().top_id == m_node->m_block_chain.get_tip_bid()) {
					m_node->m_log(logging::TRACE) << "DownloaderV11::on_idle sync_transactions to " << who.first->get_address() << std::endl;
					m_node->sync_transactions(who.first);
					break;
				}
			}
	}

	return !m_download_chain.empty() && m_download_chain.front().status == DownloadCell::PREPARED;
}

void Node::DownloaderV11::on_download_timer() {
	m_download_timer.once(SYNC_TIMEOUT / 8);  // just several ticks per SYNC_TIMEOUT
	auto idea_now = std::chrono::steady_clock::now();
	if (!m_download_chain.empty() && m_download_chain.front().status == DownloadCell::DOWNLOADING &&
	    m_download_chain.front().downloading_client && m_download_chain.front().protect_from_disconnect &&
	    std::chrono::duration_cast<std::chrono::seconds>(idea_now - m_download_chain.front().request_time).count() >
	        SYNC_TIMEOUT) {
		auto who = m_download_chain.front().downloading_client;
		m_node->m_peer_db.delay_connection_attempt(who->get_address(), m_node->m_p2p.get_local_time());
		std::cout << "Disconnecting protected slacker " << who->get_address() << std::endl;
		m_node->m_log(logging::TRACE) << "DownloaderV11::on_download_timer disconnecting protected slacker " << who->get_address() << std::endl;
		who->disconnect(std::string());
	}
}

void Node::DownloaderV11::advance_download(Hash last_downloaded_block) {
	if (m_node->m_block_chain_reader1 || m_node->m_block_chain_reader2 ||
	    m_block_chain.get_tip_height() < m_block_chain.internal_import_known_height())
		return;
	const size_t TOTAL_DOWNLOAD_BLOCKS = 400;   // TODO - dynamic count
	const size_t TOTAL_DOWNLOAD_WINDOW = 2000;  // TODO - dynamic count
	while (m_download_chain.size() < TOTAL_DOWNLOAD_WINDOW && !m_chain.empty()) {
		m_download_chain.push_back(DownloadCell());
		m_download_chain.back().bid             = m_chain.front();
		m_download_chain.back().expected_height = m_chain_start_height;
		m_download_chain.back().bid_source      = chain_source;
		m_chain.pop_front();
		m_chain_start_height += 1;
	}
	advance_chain();

	while (m_who_downloaded_block.size() > TOTAL_DOWNLOAD_BLOCKS)
		m_who_downloaded_block.pop_front();
	std::map<P2PClientJetcash *, size_t> who_downloaded_counter;
	for (auto lit = m_who_downloaded_block.begin(); lit != m_who_downloaded_block.end(); ++lit)
		who_downloaded_counter[*lit] += 1;
	auto idea_now = std::chrono::steady_clock::now();
	for (auto &&dc : m_download_chain) {
		if (dc.status != DownloadCell::DOWNLOADING || dc.downloading_client)
			continue;  // downloaded or downloading
		if (total_downloading_blocks >= TOTAL_DOWNLOAD_BLOCKS)
			break;
		P2PClientJetcash *ready_client = nullptr;
		size_t ready_counter            = std::numeric_limits<size_t>::max();
		size_t ready_speed              = 1;
		for (auto &&who : m_good_clients) {
			size_t speed =
			    std::max<size_t>(1, std::min<size_t>(TOTAL_DOWNLOAD_BLOCKS / 4, who_downloaded_counter[who.first]));
			// We clamp speed so that if even 1 downloaded all blocks, we will give
			// small % of blocks to other peers
			if (who.second * ready_speed < ready_counter * speed &&
			    who.first->get_last_received_sync_data().current_height >= dc.expected_height) {
				ready_client  = who.first;
				ready_counter = who.second;
				ready_speed   = speed;
			}
		}
		if (!ready_client)
			continue;  // Bad situation... Can be fixed only in new p2p protocol
		dc.downloading_client = ready_client;
		dc.request_time       = std::chrono::steady_clock::now();
		m_good_clients[ready_client] += 1;
		total_downloading_blocks += 1;
		NOTIFY_REQUEST_GET_OBJECTS::request msg;
		msg.blocks.push_back(dc.bid);
//		auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(idea_now - log_request_timestamp).count() > 1000) {
			log_request_timestamp = idea_now;
			std::cout << "Requesting block " << dc.expected_height << " from " << ready_client->get_address()
			          << std::endl;
		}
		m_node->m_log(logging::TRACE) << "DownloaderV11::advance_download requesting block " << dc.expected_height << " hash=" << common::pod_to_hex(dc.bid) << " from " << ready_client->get_address()
				  << std::endl;
		BinaryArray raw_msg =
		    LevinProtocol::send_message(NOTIFY_REQUEST_GET_OBJECTS::ID, LevinProtocol::encode(msg), false);
		ready_client->send(std::move(raw_msg));
	}
	const bool bad_timeout = !m_download_chain.empty() && m_download_chain.front().status == DownloadCell::DOWNLOADING &&
	    m_download_chain.front().downloading_client && !m_download_chain.front().protect_from_disconnect &&
	    std::chrono::duration_cast<std::chrono::seconds>(idea_now - m_download_chain.front().request_time).count() >
	        2 * SYNC_TIMEOUT;
	if( bad_timeout )
		std::cout << "Aha" << std::endl;
	const bool bad_relatively_slow = total_downloading_blocks < TOTAL_DOWNLOAD_BLOCKS && m_download_chain.size() >= TOTAL_DOWNLOAD_WINDOW &&
	    m_good_clients.size() > 1 && m_download_chain.front().status == DownloadCell::DOWNLOADING &&
	    m_download_chain.front().downloading_client && !m_download_chain.front().protect_from_disconnect;
	if (bad_relatively_slow || bad_timeout) {
		auto who = m_download_chain.front().downloading_client;
		for (auto &&dc : m_download_chain)
			if (dc.downloading_client == who)
				dc.protect_from_disconnect = true;
		m_node->m_peer_db.delay_connection_attempt(who->get_address(), m_node->m_p2p.get_local_time());
		std::cout << "Disconnecting slacker " << who->get_address() << std::endl;
		m_node->m_log(logging::TRACE) << "DownloaderV11::advance_download disconnecting slacker " << who->get_address() << std::endl;
		who->disconnect(std::string());
	}
}