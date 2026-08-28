#pragma once

#include <string>
#include <vector>
#include "ecs/PropertyReflection.h"

namespace rigkit {
namespace ecs {

/**
 * @brief One IPv4 address on the LAN - data only.
 * @details Written by **rigNetScan** from TCP connect results. `alive` is true
 * when a port opened or refused (host answered). Timeout-only rows stay dead.
 * Prefer one entity per address. Host extension (`x.rigkit.net_host`).
 */
struct CNetHost {
	std::string ip;
	std::string hostname;
	bool alive = false;
	int rttMs = -1;			 ///< First TCP answer; -1 if none
	std::string openPorts; ///< Comma-separated ports that accepted connect

	std::vector<sProp> GetProperties() {
		return {{0, "IP", EPT_STRING, &ip},
				{1, "Hostname", EPT_STRING, &hostname},
				{2, "Alive", EPT_BOOL, &alive},
				{3, "RTT Ms", EPT_INT, &rttMs},
				{4, "Open Ports", EPT_STRING, &openPorts}};
	}
};

} // namespace ecs
} // namespace rigkit
