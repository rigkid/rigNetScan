#include "NetIo.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rigkit {
namespace netscan {
namespace {

int64_t nowMs() {
	using clock = std::chrono::steady_clock;
	return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
		.count();
}

#ifdef _WIN32
SOCKET asSock(uintptr_t s) {
	return static_cast<SOCKET>(s);
}
uintptr_t fromSock(SOCKET s) {
	return static_cast<uintptr_t>(s);
}
const uintptr_t kInvalid = static_cast<uintptr_t>(INVALID_SOCKET);
#else
int asSock(uintptr_t s) {
	return static_cast<int>(s);
}
uintptr_t fromSock(int s) {
	return static_cast<uintptr_t>(s);
}
const uintptr_t kInvalid = static_cast<uintptr_t>(-1);
#endif

bool setNonBlock(uintptr_t sock) {
#ifdef _WIN32
	u_long nb = 1;
	return ioctlsocket(asSock(sock), FIONBIO, &nb) == 0;
#else
	const int flags = fcntl(asSock(sock), F_GETFL, 0);
	if (flags < 0) {
		return false;
	}
	return fcntl(asSock(sock), F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

} // namespace

bool TcpConnect::startup() {
#ifdef _WIN32
	static bool once = false;
	static bool ok = false;
	if (!once) {
		once = true;
		WSADATA wsa{};
		ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
	}
	return ok;
#else
	return true;
#endif
}

TcpConnect::~TcpConnect() {
	close();
}

TcpConnect::TcpConnect(TcpConnect&& other) noexcept {
	*this = std::move(other);
}

TcpConnect& TcpConnect::operator=(TcpConnect&& other) noexcept {
	if (this == &other) {
		return *this;
	}
	close();
	m_open = other.m_open;
	m_ip = other.m_ip;
	m_port = other.m_port;
	m_startMs = other.m_startMs;
	m_sock = other.m_sock;
	other.m_open = false;
	other.reset();
	return *this;
}

void TcpConnect::reset() {
	m_open = false;
	m_ip = 0;
	m_port = 0;
	m_startMs = 0;
	m_sock = kInvalid;
}

void TcpConnect::close() {
	if (!m_open) {
		reset();
		return;
	}
	closeSocket(m_sock);
	reset();
}

int TcpConnect::elapsedMs() const {
	if (!m_open) {
		return 0;
	}
	return static_cast<int>(nowMs() - m_startMs);
}

bool TcpConnect::begin(uint32_t ipv4Host, int port) {
	close();
	if (!startup()) {
		return false;
	}
	if (port < 1 || port > 65535) {
		return false;
	}

#ifdef _WIN32
	SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		return false;
	}
	m_sock = fromSock(s);
#else
	int s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s < 0) {
		return false;
	}
	m_sock = fromSock(s);
#endif
	if (!setNonBlock(m_sock)) {
		closeSocket(m_sock);
		reset();
		return false;
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(port));
	addr.sin_addr.s_addr = htonl(ipv4Host);

#ifdef _WIN32
	const int rc = ::connect(asSock(m_sock), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
		closeSocket(m_sock);
		reset();
		return false;
	}
#else
	const int rc = ::connect(asSock(m_sock), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	if (rc != 0 && errno != EINPROGRESS) {
		closeSocket(m_sock);
		reset();
		return false;
	}
#endif
	m_open = true;
	m_ip = ipv4Host;
	m_port = port;
	m_startMs = nowMs();
	return true;
}

ProbeResult TcpConnect::poll(int timeoutMs) {
	if (!m_open) {
		return ProbeResult::Error;
	}
	if (timeoutMs < 1) {
		timeoutMs = 1;
	}
	if (elapsedMs() >= timeoutMs) {
		close();
		return ProbeResult::Timeout;
	}

#ifdef _WIN32
	fd_set writeSet;
	fd_set errSet;
	FD_ZERO(&writeSet);
	FD_ZERO(&errSet);
	SOCKET s = asSock(m_sock);
	FD_SET(s, &writeSet);
	FD_SET(s, &errSet);
	timeval tv{};
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	const int n = ::select(0, nullptr, &writeSet, &errSet, &tv);
	if (n == 0) {
		return ProbeResult::Pending;
	}
	if (n < 0) {
		close();
		return ProbeResult::Error;
	}
	int soErr = 0;
	int len = sizeof(soErr);
	if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len) != 0) {
		close();
		return ProbeResult::Error;
	}
	close();
	if (soErr == 0) {
		return ProbeResult::Open;
	}
	if (soErr == WSAECONNREFUSED) {
		return ProbeResult::Refused;
	}
	if (soErr == WSAETIMEDOUT) {
		return ProbeResult::Timeout;
	}
	return ProbeResult::Error;
#else
	pollfd pfd{};
	pfd.fd = asSock(m_sock);
	pfd.events = POLLOUT;
	const int n = ::poll(&pfd, 1, 0);
	if (n == 0) {
		return ProbeResult::Pending;
	}
	if (n < 0) {
		close();
		return ProbeResult::Error;
	}
	int soErr = 0;
	socklen_t len = sizeof(soErr);
	if (getsockopt(asSock(m_sock), SOL_SOCKET, SO_ERROR, &soErr, &len) != 0) {
		close();
		return ProbeResult::Error;
	}
	close();
	if (soErr == 0) {
		return ProbeResult::Open;
	}
	if (soErr == ECONNREFUSED) {
		return ProbeResult::Refused;
	}
	if (soErr == ETIMEDOUT) {
		return ProbeResult::Timeout;
	}
	return ProbeResult::Error;
#endif
}

bool parseIpv4(const std::string& text, uint32_t& outHost) {
	if (text.empty()) {
		return false;
	}
	in_addr addr{};
#ifdef _WIN32
	if (InetPtonA(AF_INET, text.c_str(), &addr) != 1) {
		return false;
	}
#else
	if (inet_pton(AF_INET, text.c_str(), &addr) != 1) {
		return false;
	}
#endif
	outHost = ntohl(addr.s_addr);
	return true;
}

std::string formatIpv4(uint32_t hostOrder) {
	in_addr addr{};
	addr.s_addr = htonl(hostOrder);
	char buf[INET_ADDRSTRLEN]{};
#ifdef _WIN32
	InetNtopA(AF_INET, &addr, buf, sizeof(buf));
#else
	inet_ntop(AF_INET, &addr, buf, sizeof(buf));
#endif
	return std::string(buf);
}

std::vector<int> parsePorts(const std::string& csv) {
	std::vector<int> ports;
	std::stringstream ss(csv);
	std::string item;
	while (std::getline(ss, item, ',')) {
		size_t a = 0;
		while (a < item.size() && (item[a] == ' ' || item[a] == '\t')) {
			++a;
		}
		size_t b = item.size();
		while (b > a && (item[b - 1] == ' ' || item[b - 1] == '\t')) {
			--b;
		}
		if (a >= b) {
			continue;
		}
		try {
			const int p = std::stoi(item.substr(a, b - a));
			if (p >= 1 && p <= 65535) {
				ports.push_back(p);
			}
		} catch (...) {
		}
	}
	std::sort(ports.begin(), ports.end());
	ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
	return ports;
}

std::string webUrl(const std::string& ip, const std::string& openPortsCsv) {
	if (ip.empty()) {
		return {};
	}
	bool http = false;
	bool https = false;
	for (int p : parsePorts(openPortsCsv)) {
		if (p == 80) {
			http = true;
		} else if (p == 443) {
			https = true;
		}
	}
	if (https) {
		return "https://" + ip + "/";
	}
	if (http) {
		return "http://" + ip + "/";
	}
	return {};
}

namespace {

void rangeFromPrefix(uint32_t ip, uint32_t prefixLen, std::string& from, std::string& to) {
	if (prefixLen > 32) {
		prefixLen = 32;
	}
	const uint32_t mask = prefixLen == 0 ? 0u : (~0u << (32u - prefixLen));
	const uint32_t network = ip & mask;
	const uint32_t broadcast = network | (~mask);
	if (prefixLen >= 31) {
		from = formatIpv4(ip);
		to = formatIpv4(ip);
		return;
	}
	from = formatIpv4(network + 1);
	to = formatIpv4(broadcast - 1);
}

bool skipIpv4(uint32_t ip) {
	if ((ip & 0xff000000u) == 0x7f000000u) {
		return true;
	}
	if ((ip & 0xffff0000u) == 0xa9fe0000u) {
		return true; // 169.254/16
	}
	return ip == 0 || ip == 0xffffffffu;
}

bool rfc1918(uint32_t ip) {
	if ((ip & 0xff000000u) == 0x0a000000u) {
		return true;
	}
	if ((ip & 0xfff00000u) == 0xac100000u) {
		return true;
	}
	return (ip & 0xffff0000u) == 0xc0a80000u;
}

int rangeScore(uint32_t ip, uint32_t prefix) {
	if (skipIpv4(ip) || prefix > 32) {
		return -1;
	}
	int s = 0;
	if (prefix >= 24 && prefix <= 30) {
		s += 100;
	} else if (prefix >= 16 && prefix <= 23) {
		s += 1;
	}
	if (rfc1918(ip)) {
		s += 10;
	}
	return s;
}

struct Ipv4Cand {
	uint32_t ip = 0;
	uint32_t prefix = 24;
	int score = -1;
};

bool pickBest(const std::vector<Ipv4Cand>& cands, std::string& from, std::string& to) {
	const Ipv4Cand* best = nullptr;
	for (const auto& c : cands) {
		if (c.score < 0) {
			continue;
		}
		if (!best || c.score > best->score ||
			(c.score == best->score && c.prefix > best->prefix)) {
			best = &c;
		}
	}
	if (!best) {
		return false;
	}
	rangeFromPrefix(best->ip, best->prefix, from, to);
	return true;
}

} // namespace

bool detectLocalIpv4Range(std::string& from, std::string& to, std::string* error) {
	if (error) {
		error->clear();
	}
	if (!TcpConnect::startup()) {
		if (error) {
			*error = "socket init failed";
		}
		return false;
	}

#ifdef _WIN32
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG size = 16 * 1024;
	std::vector<unsigned char> buf(size);
	auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
	ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
	if (rc == ERROR_BUFFER_OVERFLOW) {
		buf.resize(size);
		addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
		rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
	}
	if (rc != NO_ERROR) {
		if (error) {
			*error = "GetAdaptersAddresses failed";
		}
		return false;
	}
	std::vector<Ipv4Cand> cands;
	for (auto* a = addrs; a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp) {
			continue;
		}
		if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
			continue;
		}
		for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
			if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) {
				continue;
			}
			auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
			Ipv4Cand c;
			c.ip = ntohl(sin->sin_addr.s_addr);
			c.prefix = u->OnLinkPrefixLength;
			c.score = rangeScore(c.ip, c.prefix);
			cands.push_back(c);
		}
	}
	if (pickBest(cands, from, to)) {
		return true;
	}
	if (error) {
		*error = "no IPv4 adapter";
	}
	return false;
#else
	ifaddrs* list = nullptr;
	if (getifaddrs(&list) != 0) {
		if (error) {
			*error = "getifaddrs failed";
		}
		return false;
	}
	std::vector<Ipv4Cand> cands;
	for (ifaddrs* a = list; a; a = a->ifa_next) {
		if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) {
			continue;
		}
		if (a->ifa_flags & IFF_LOOPBACK) {
			continue;
		}
		if (!(a->ifa_flags & IFF_UP)) {
			continue;
		}
		auto* sin = reinterpret_cast<sockaddr_in*>(a->ifa_addr);
		Ipv4Cand c;
		c.ip = ntohl(sin->sin_addr.s_addr);
		c.prefix = 24;
		if (a->ifa_netmask && a->ifa_netmask->sa_family == AF_INET) {
			const uint32_t mask =
				ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr);
			c.prefix = 0;
			for (uint32_t bit = 0x80000000u; bit && (mask & bit); bit >>= 1) {
				++c.prefix;
			}
		}
		c.score = rangeScore(c.ip, c.prefix);
		cands.push_back(c);
	}
	freeifaddrs(list);
	if (pickBest(cands, from, to)) {
		return true;
	}
	if (error) {
		*error = "no IPv4 adapter";
	}
	return false;
#endif
}

std::string reverseName(uint32_t ipv4Host) {
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(ipv4Host);
	char host[NI_MAXHOST]{};
	const int rc = getnameinfo(reinterpret_cast<sockaddr*>(&addr), sizeof(addr), host, sizeof(host),
							   nullptr, 0, NI_NAMEREQD);
	if (rc != 0) {
		return {};
	}
	return std::string(host);
}

bool bindLoopbackListen(int portHint, int& portOut, uintptr_t& sockOut) {
	sockOut = kInvalid;
	portOut = 0;
	if (!TcpConnect::startup()) {
		return false;
	}
#ifdef _WIN32
	SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		return false;
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(static_cast<uint16_t>(portHint < 0 ? 0 : portHint));
	if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
		closesocket(s);
		return false;
	}
	if (::listen(s, 8) != 0) {
		closesocket(s);
		return false;
	}
	sockaddr_in bound{};
	int len = sizeof(bound);
	if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
		closesocket(s);
		return false;
	}
	portOut = ntohs(bound.sin_port);
	sockOut = fromSock(s);
	return true;
#else
	int s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s < 0) {
		return false;
	}
	int yes = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(static_cast<uint16_t>(portHint < 0 ? 0 : portHint));
	if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
		::close(s);
		return false;
	}
	if (::listen(s, 8) != 0) {
		::close(s);
		return false;
	}
	sockaddr_in bound{};
	socklen_t len = sizeof(bound);
	if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
		::close(s);
		return false;
	}
	portOut = ntohs(bound.sin_port);
	sockOut = fromSock(s);
	return true;
#endif
}

void closeSocket(uintptr_t sock) {
	if (sock == kInvalid) {
		return;
	}
#ifdef _WIN32
	closesocket(asSock(sock));
#else
	::close(asSock(sock));
#endif
}

} // namespace netscan
} // namespace rigkit
