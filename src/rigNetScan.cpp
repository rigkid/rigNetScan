#include "rigNetScan.h"

#include "core/pack/PackRegistry.h"
#include "core/RigKitEngine.h"
#include "CNetHost.h"
#include "CNetScan.h"
#include "NetIo.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <spdlog/spdlog.h>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rigkit {
namespace {

constexpr int kMaxRange = 4096;
constexpr int kMaxInFlight = 64;

std::string joinPorts(const std::vector<int>& ports) {
	std::ostringstream oss;
	for (size_t i = 0; i < ports.size(); ++i) {
		if (i) {
			oss << ',';
		}
		oss << ports[i];
	}
	return oss.str();
}

} // namespace

struct rigNetScan::ScanState {
	struct HostAccum {
		entt::entity entity = entt::null;
		int remaining = 0;
		int rttMs = -1;
		bool alive = false;
		bool named = false;
		std::vector<int> openPorts;
	};

	bool scanning = false;
	std::deque<std::pair<uint32_t, int>> queue;
	std::vector<netscan::TcpConnect> inflight;
	std::unordered_map<uint32_t, HostAccum> hosts;
	int ipsDone = 0;
	int ipsTotal = 0;
	int timeoutMs = 200;
	int maxInFlight = 24;
	std::future<std::pair<uint32_t, std::string>> nameJob;
};

rigNetScan::rigNetScan() : IPack("rigNetScan"), m_state(std::make_unique<ScanState>()) {}

rigNetScan::~rigNetScan() {
	cleanup();
}

bool rigNetScan::init() {
	if (!netscan::TcpConnect::startup()) {
		m_lastError = "socket init failed";
		spdlog::error("[rigNetScan] {}", m_lastError);
		return false;
	}
	spdlog::info("[rigNetScan] init");
	return true;
}

void rigNetScan::ensureScanEntity() {
	auto* engine = getEngine();
	if (!engine || !engine->getECSManager()) {
		return;
	}
	auto* ecs = engine->getECSManager();
	if (m_scanEntity != entt::null && ecs->registry().valid(m_scanEntity)) {
		return;
	}
	if (auto existing = ecs->findEntity("netscan"); existing != entt::null) {
		m_scanEntity = existing;
	} else {
		m_scanEntity = ecs->createEntity("netscan");
	}
	if (!ecs->hasComponent<ecs::CNetScan>(m_scanEntity)) {
		ecs->addComponent<ecs::CNetScan>(m_scanEntity, {});
	}
}

void rigNetScan::setup() {
	auto* engine = getEngine();
	if (!engine || !engine->getECSManager()) {
		return;
	}
	auto* ecs = engine->getECSManager();
	ecs->registerComponent<ecs::CNetScan>("NetScan", true);
	ecs->registerComponent<ecs::CNetHost>("NetHost", true);
	ensureScanEntity();
	fillLocalRange();
	if (auto* job = tryScan()) {
		spdlog::info("[rigNetScan] setup - range {} .. {} ports {}", job->from, job->to,
					 job->ports);
	}
}

void rigNetScan::cleanup() {
	abortScan();
}

ecs::CNetScan* rigNetScan::tryScan() {
	ensureScanEntity();
	auto* engine = getEngine();
	if (!engine || !engine->getECSManager()) {
		return nullptr;
	}
	auto* ecs = engine->getECSManager();
	if (m_scanEntity == entt::null || !ecs->registry().valid(m_scanEntity) ||
		!ecs->hasComponent<ecs::CNetScan>(m_scanEntity)) {
		return nullptr;
	}
	return &ecs->getComponent<ecs::CNetScan>(m_scanEntity);
}

const ecs::CNetScan* rigNetScan::tryScan() const {
	return const_cast<rigNetScan*>(this)->tryScan();
}

bool rigNetScan::fillLocalRange() {
	auto* job = tryScan();
	if (!job) {
		return false;
	}
	std::string err;
	if (!netscan::detectLocalIpv4Range(job->from, job->to, &err)) {
		m_lastError = err.empty() ? "no local IPv4 range" : err;
		job->from = "127.0.0.1";
		job->to = "127.0.0.1";
		job->status = "loopback (no LAN adapter)";
		return false;
	}
	job->status = "idle";
	m_lastError.clear();
	return true;
}

void rigNetScan::clearHosts() {
	auto* engine = getEngine();
	auto* ecs = engine ? engine->getECSManager() : nullptr;
	if (!ecs) {
		return;
	}
	std::vector<entt::entity> kill;
	for (auto e : ecs->view<ecs::CNetHost>()) {
		kill.push_back(e);
	}
	for (auto e : kill) {
		ecs->destroyEntity(e);
	}
	m_state->hosts.clear();
}

void rigNetScan::abortScan() {
	if (!m_state) {
		return;
	}
	if (m_state->nameJob.valid()) {
		m_state->nameJob.wait();
	}
	for (auto& p : m_state->inflight) {
		p.close();
	}
	m_state->inflight.clear();
	m_state->queue.clear();
	m_state->scanning = false;
}

void rigNetScan::finishScan() {
	abortScan();
	auto* job = tryScan();
	if (!job) {
		return;
	}
	job->running = false;
	job->done = m_state->ipsDone;
	job->total = m_state->ipsTotal;
	job->aliveCount = 0;
	for (const auto& kv : m_state->hosts) {
		if (kv.second.alive) {
			++job->aliveCount;
		}
	}
	job->status = "done";
}

bool rigNetScan::beginScan() {
	auto* job = tryScan();
	if (!job) {
		m_lastError = "no scan entity";
		return false;
	}
	m_lastError.clear();
	if (job->timeoutMs < 20) {
		job->timeoutMs = 20;
	}
	if (job->maxInFlight < 1) {
		job->maxInFlight = 1;
	}
	if (job->maxInFlight > kMaxInFlight) {
		job->maxInFlight = kMaxInFlight;
	}

	uint32_t from = 0;
	uint32_t to = 0;
	if (!netscan::parseIpv4(job->from, from) || !netscan::parseIpv4(job->to, to)) {
		m_lastError = "invalid IPv4 range";
		job->running = false;
		job->status = m_lastError;
		return false;
	}
	if (from > to) {
		std::swap(from, to);
		job->from = netscan::formatIpv4(from);
		job->to = netscan::formatIpv4(to);
	}
	const auto ports = netscan::parsePorts(job->ports);
	if (ports.empty()) {
		m_lastError = "no TCP ports";
		job->running = false;
		job->status = m_lastError;
		return false;
	}

	uint64_t count = static_cast<uint64_t>(to) - static_cast<uint64_t>(from) + 1;
	if (count > static_cast<uint64_t>(kMaxRange)) {
		to = from + static_cast<uint32_t>(kMaxRange - 1);
		job->to = netscan::formatIpv4(to);
		count = kMaxRange;
		spdlog::warn("[rigNetScan] range capped at {} addresses", kMaxRange);
	}

	abortScan();
	clearHosts();
	m_state->queue.clear();
	m_state->hosts.clear();
	m_state->ipsDone = 0;
	m_state->ipsTotal = static_cast<int>(count);
	m_state->timeoutMs = job->timeoutMs;
	m_state->maxInFlight = job->maxInFlight;

	for (uint32_t ip = from; ip <= to; ++ip) {
		auto& acc = m_state->hosts[ip];
		acc.remaining = static_cast<int>(ports.size());
		for (int p : ports) {
			m_state->queue.emplace_back(ip, p);
		}
		if (ip == 0xffffffffu) {
			break;
		}
	}

	job->done = 0;
	job->total = m_state->ipsTotal;
	job->aliveCount = 0;
	job->status = "scanning";
	m_state->scanning = true;
	spdlog::info("[rigNetScan] scan {} .. {} ({} hosts, {} ports)", job->from, job->to,
				 m_state->ipsTotal, ports.size());
	return true;
}

void rigNetScan::writeHost(uint32_t ip) {
	auto* engine = getEngine();
	auto* ecs = engine ? engine->getECSManager() : nullptr;
	if (!ecs) {
		return;
	}
	auto it = m_state->hosts.find(ip);
	if (it == m_state->hosts.end()) {
		return;
	}
	auto& acc = it->second;
	if (acc.entity == entt::null || !ecs->registry().valid(acc.entity)) {
		const std::string name = std::string("nethost-") + netscan::formatIpv4(ip);
		acc.entity = ecs->createEntity(name);
		ecs::CNetHost row;
		row.ip = netscan::formatIpv4(ip);
		ecs->addComponent(acc.entity, row);
	}
	auto& row = ecs->getComponent<ecs::CNetHost>(acc.entity);
	row.ip = netscan::formatIpv4(ip);
	row.alive = acc.alive;
	row.rttMs = acc.rttMs;
	row.openPorts = joinPorts(acc.openPorts);
}

void rigNetScan::applyResult(uint32_t ip, int port, int resultRaw, int elapsedMs) {
	const auto result = static_cast<netscan::ProbeResult>(resultRaw);
	auto it = m_state->hosts.find(ip);
	if (it == m_state->hosts.end()) {
		return;
	}
	auto& acc = it->second;
	if (result == netscan::ProbeResult::Open || result == netscan::ProbeResult::Refused) {
		acc.alive = true;
		if (acc.rttMs < 0 || elapsedMs < acc.rttMs) {
			acc.rttMs = elapsedMs;
		}
	}
	if (result == netscan::ProbeResult::Open) {
		if (std::find(acc.openPorts.begin(), acc.openPorts.end(), port) == acc.openPorts.end()) {
			acc.openPorts.push_back(port);
			std::sort(acc.openPorts.begin(), acc.openPorts.end());
		}
	}
	if (acc.remaining > 0) {
		--acc.remaining;
	}
	writeHost(ip);
	if (acc.remaining <= 0) {
		++m_state->ipsDone;
	}
}

void rigNetScan::pumpNames() {
	auto& st = *m_state;
	if (st.nameJob.valid()) {
		if (st.nameJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
			return;
		}
		const auto result = st.nameJob.get();
		auto it = st.hosts.find(result.first);
		if (it == st.hosts.end()) {
			return;
		}
		auto* ecs = getEngine()->getECSManager();
		if (!ecs || it->second.entity == entt::null || !ecs->registry().valid(it->second.entity)) {
			return;
		}
		ecs->getComponent<ecs::CNetHost>(it->second.entity).hostname = result.second;
	}

	const auto* job = tryScan();
	if (!job || !job->resolveNames) {
		return;
	}
	auto* ecs = getEngine()->getECSManager();
	if (!ecs) {
		return;
	}
	for (auto& kv : st.hosts) {
		auto& acc = kv.second;
		if (!acc.alive || acc.named) {
			continue;
		}
		if (acc.entity == entt::null || !ecs->registry().valid(acc.entity)) {
			continue;
		}
		if (!ecs->getComponent<ecs::CNetHost>(acc.entity).hostname.empty()) {
			acc.named = true;
			continue;
		}
		acc.named = true;
		const uint32_t ip = kv.first;
		st.nameJob = std::async(std::launch::async, [ip]() {
			return std::pair<uint32_t, std::string>{ip, netscan::reverseName(ip)};
		});
		return;
	}
}

void rigNetScan::pumpProbes() {
	auto& st = *m_state;
	while (static_cast<int>(st.inflight.size()) < st.maxInFlight && !st.queue.empty()) {
		const auto [ip, port] = st.queue.front();
		st.queue.pop_front();
		netscan::TcpConnect probe;
		if (!probe.begin(ip, port)) {
			applyResult(ip, port, static_cast<int>(netscan::ProbeResult::Error), 0);
			continue;
		}
		st.inflight.push_back(std::move(probe));
	}

	for (size_t i = 0; i < st.inflight.size();) {
		auto& probe = st.inflight[i];
		const uint32_t ip = probe.ip();
		const int port = probe.port();
		const int elapsed = probe.elapsedMs();
		const auto result = probe.poll(st.timeoutMs);
		if (result == netscan::ProbeResult::Pending) {
			++i;
			continue;
		}
		applyResult(ip, port, static_cast<int>(result), elapsed);
		st.inflight[i] = std::move(st.inflight.back());
		st.inflight.pop_back();
	}
}

void rigNetScan::update(float) {
	auto* job = tryScan();
	if (!job || !m_state) {
		return;
	}
	if (job->running && !m_state->scanning) {
		if (!beginScan()) {
			return;
		}
	}
	if (!job->running && m_state->scanning) {
		finishScan();
		if (auto* j = tryScan()) {
			j->status = "stopped";
		}
	}
	if (m_state->scanning) {
		pumpProbes();
		job = tryScan();
		if (!job) {
			return;
		}
		job->done = m_state->ipsDone;
		job->total = m_state->ipsTotal;
		job->aliveCount = 0;
		for (const auto& kv : m_state->hosts) {
			if (kv.second.alive) {
				++job->aliveCount;
			}
		}
		if (m_state->queue.empty() && m_state->inflight.empty()) {
			finishScan();
		}
	}
	pumpNames();
}

bool rigNetScan::smokeLoopback() {
	int port = 0;
	uintptr_t listen = static_cast<uintptr_t>(~0);
	if (!netscan::bindLoopbackListen(0, port, listen)) {
		m_lastError = "smoke listen bind failed";
		return false;
	}

	auto* job = tryScan();
	if (!job) {
		netscan::closeSocket(listen);
		m_lastError = "smoke: no scan entity";
		return false;
	}
	job->from = "127.0.0.1";
	job->to = "127.0.0.1";
	job->ports = std::to_string(port);
	job->timeoutMs = 400;
	job->maxInFlight = 4;
	job->resolveNames = false;
	job->running = true;

	bool ok = false;
	for (int i = 0; i < 400; ++i) {
		update(0.016f);
		job = tryScan();
		if (!job || !job->running) {
			break;
		}
	}

	auto* engine = getEngine();
	auto* ecs = engine ? engine->getECSManager() : nullptr;
	if (ecs) {
		const std::string want = std::to_string(port);
		for (auto e : ecs->view<ecs::CNetHost>()) {
			auto& row = ecs->getComponent<ecs::CNetHost>(e);
			if (row.ip == "127.0.0.1" && row.alive && row.openPorts == want) {
				ok = true;
				break;
			}
		}
	}
	netscan::closeSocket(listen);
	if (!ok) {
		m_lastError = "smoke: 127.0.0.1 did not show open port " + std::to_string(port);
		spdlog::error("[rigNetScan] {}", m_lastError);
	} else {
		m_lastError.clear();
		spdlog::info("[rigNetScan] smoke-netscan OK (127.0.0.1:{})", port);
	}
	return ok;
}

} // namespace rigkit

namespace {
struct rigNetScanRegistrar {
	rigNetScanRegistrar() {
		rigkit::PackRegistry::instance().addFactory("rigNetScan", []() {
			return std::shared_ptr<rigkit::IPack>(std::make_shared<rigkit::rigNetScan>());
		});
	}
};
static rigNetScanRegistrar rigNetScan_auto_reg;
} // namespace
