#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "core/pack/IPack.h"
#include "ecs/MEcs.h"

namespace rigkit {

namespace ecs {
struct CNetScan;
struct CNetHost;
}

/**
 * @brief LAN TCP connect scan for controller troubleshooting.
 * @details Owns sockets. POD is @ref ecs::CNetScan (job) and @ref ecs::CNetHost
 * (one row per IPv4). ICMP is not the floor — TCP connect works on Pi without
 * CAP_NET_RAW. Set `CNetScan::running` to start or stop; probes run in Update.
 *
 * No `rig.*` schema yet — travel as `x.rigkit.net_scan` / `x.rigkit.net_host`.
 */
class rigNetScan : public IPack {
  public:
	rigNetScan();
	~rigNetScan() override;

	bool init() override;
	void setup() override;
	void update(float deltaTime) override;
	void cleanup() override;

	entt::entity scanEntity() const { return m_scanEntity; }

	/** @brief Job POD when engine + entity exist; null otherwise. */
	ecs::CNetScan* tryScan();
	const ecs::CNetScan* tryScan() const;

	/** @brief Write from/to from the first non-loopback IPv4 adapter. */
	bool fillLocalRange();

	/** @brief Listen on loopback, scan 127.0.0.1, require an open port. */
	bool smokeLoopback();

	const std::string& lastError() const { return m_lastError; }

  private:
	struct ScanState;

	void ensureScanEntity();
	void abortScan();
	void finishScan();
	bool beginScan();
	void pumpProbes();
	void pumpNames();
	void applyResult(uint32_t ip, int port, int result, int elapsedMs);
	void writeHost(uint32_t ip);
	void clearHosts();

	entt::entity m_scanEntity = entt::null;
	std::string m_lastError;
	std::unique_ptr<ScanState> m_state;
};

} // namespace rigkit
