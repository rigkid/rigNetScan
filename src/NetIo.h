#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rigkit {
namespace netscan {

/** @brief Outcome of one non-blocking TCP connect. */
enum class ProbeResult { Pending, Open, Refused, Timeout, Error };

/**
 * @brief One in-flight TCP connect (Winsock / BSD).
 * @details Handle stays here — never on CNetHost. Call @ref startup once
 * before the first @ref begin (WSAStartup on Windows).
 */
class TcpConnect {
  public:
	TcpConnect() = default;
	~TcpConnect();

	TcpConnect(const TcpConnect&) = delete;
	TcpConnect& operator=(const TcpConnect&) = delete;
	TcpConnect(TcpConnect&& other) noexcept;
	TcpConnect& operator=(TcpConnect&& other) noexcept;

	/** @brief Process-wide socket init. Safe to call more than once. */
	static bool startup();

	/**
	 * @brief Start a non-blocking connect.
	 * @param ipv4Host IPv4 in host byte order.
	 */
	bool begin(uint32_t ipv4Host, int port);

	/** @brief Poll; `timeoutMs` is wall time since @ref begin, not a blocking wait. */
	ProbeResult poll(int timeoutMs);

	int elapsedMs() const;
	void close();
	bool active() const { return m_open; }

	uint32_t ip() const { return m_ip; }
	int port() const { return m_port; }

  private:
	void reset();

	bool m_open = false;
	uint32_t m_ip = 0;
	int m_port = 0;
	int64_t m_startMs = 0;
	uintptr_t m_sock = static_cast<uintptr_t>(~0);
};

bool parseIpv4(const std::string& text, uint32_t& outHost);
std::string formatIpv4(uint32_t hostOrder);
std::vector<int> parsePorts(const std::string& csv);

/**
 * @brief http(s) URL when port 80 or 443 accepted connect.
 * @details 443 → `https://ip/` ; else 80 → `http://ip/`. Empty when neither.
 */
std::string webUrl(const std::string& ip, const std::string& openPortsCsv);

/**
 * @brief First non-loopback IPv4 interface as a usable scan range.
 * @details Uses the interface prefix (typically .1 .. .254 on a /24).
 * @return false when no usable adapter exists; `error` is set when provided.
 */
bool detectLocalIpv4Range(std::string& from, std::string& to, std::string* error = nullptr);

/** @brief Reverse DNS; may block. Empty string when none. */
std::string reverseName(uint32_t ipv4Host);

/**
 * @brief Bind a loopback TCP listener (smoke).
 * @param portOut Assigned port when `portHint` is 0.
 */
bool bindLoopbackListen(int portHint, int& portOut, uintptr_t& sockOut);
void closeSocket(uintptr_t sock);

} // namespace netscan
} // namespace rigkit
