#pragma once

#include <string>
#include <vector>
#include "ecs/PropertyReflection.h"

namespace rigkit {
namespace ecs {

/**
 * @brief One LAN scan job - data only.
 * @details Range and ports are the intent. `running` / `done` / `total` /
 * `status` are written by **rigNetScan** while the probe runs. Sockets stay
 * in the pack. Host extension until RigWorks names a LAN-host schema
 * (`x.rigkit.net_scan`).
 */
struct CNetScan {
	std::string from = "127.0.0.1";
	std::string to = "127.0.0.1";
	std::string ports = "80,443,8000"; ///< Comma-separated TCP ports
	int timeoutMs = 200;
	int maxInFlight = 24; ///< Concurrent connects; keep small on Pi
	bool resolveNames = false;
	bool running = false;
	int done = 0;
	int total = 0;
	int aliveCount = 0;
	std::string status = "idle";

	std::vector<sProp> GetProperties() {
		return {{0, "From", EPT_STRING, &from},
				{1, "To", EPT_STRING, &to},
				{2, "Ports", EPT_STRING, &ports},
				{3, "Timeout Ms", EPT_INT, &timeoutMs},
				{4, "Max In Flight", EPT_INT, &maxInFlight},
				{5, "Resolve Names", EPT_BOOL, &resolveNames},
				{6, "Running", EPT_BOOL, &running},
				{7, "Done", EPT_INT, &done},
				{8, "Total", EPT_INT, &total},
				{9, "Alive", EPT_INT, &aliveCount},
				{10, "Status", EPT_STRING, &status}};
	}
};

} // namespace ecs
} // namespace rigkit
