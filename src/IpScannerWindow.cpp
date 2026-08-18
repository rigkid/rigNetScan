#include "IpScannerWindow.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <imgui.h>
#include "CNetHost.h"
#include "CNetScan.h"
#include "NetIo.h"
#include "core/RigKitEngine.h"
#include "core/pack/MPack.h"
#include "ecs/MEcs.h"
#include "rigNetScan.h"

namespace rigkit {
namespace {

bool inputTextStd(const char* label, std::string& value) {
	char buf[256];
	std::strncpy(buf, value.c_str(), sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	if (ImGui::InputText(label, buf, sizeof(buf))) {
		value = buf;
		return true;
	}
	return false;
}

float textFieldWidth(const char* sample) {
	return ImGui::CalcTextSize(sample).x + ImGui::GetStyle().FramePadding.x * 2.f + 8.f;
}

struct HostRow {
	uint32_t ipn = 0;
	std::string ip;
	std::string hostname;
	bool alive = false;
	int rttMs = -1;
	std::string openPorts;
	std::string url;
};

} // namespace

IpScannerWindow::IpScannerWindow() : IWindow(kTitle, 0) {}

void IpScannerWindow::renderContents() {
	auto* engine = getEngine();
	if (!engine || !engine->getPackManager() || !engine->getECSManager()) {
		ImGui::TextUnformatted("Engine not ready");
		return;
	}
	auto scanPack = engine->getPackManager()->getPack<rigNetScan>();
	if (!scanPack) {
		ImGui::TextDisabled("rigNetScan not loaded");
		return;
	}
	auto* job = scanPack->tryScan();
	if (!job) {
		ImGui::TextDisabled("rigNetScan job not ready");
		return;
	}

	auto* ecs = engine->getECSManager();
	const float ipW = textFieldWidth("255.255.255.255");
	const float portsW = textFieldWidth("80,443,8000,8080,9000");
	const float timeoutW = textFieldWidth("99999");

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("From");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ipW);
	inputTextStd("##from", job->from);
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("to");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ipW);
	inputTextStd("##to", job->to);
	ImGui::SameLine();
	if (ImGui::Button("Local subnet")) {
		scanPack->fillLocalRange();
	}

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Ports");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(portsW);
	inputTextStd("##ports", job->ports);
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Timeout ms");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(timeoutW);
	ImGui::InputInt("##timeout", &job->timeoutMs, 0, 0);
	if (job->timeoutMs < 20) {
		job->timeoutMs = 20;
	}

	if (!job->running) {
		if (ImGui::Button("Scan")) {
			job->running = true;
		}
	} else {
		if (ImGui::Button("Stop")) {
			job->running = false;
		}
	}
	ImGui::SameLine();
	ImGui::Checkbox("Alive only", &m_aliveOnly);
	ImGui::SameLine();
	ImGui::Checkbox("Resolve names", &job->resolveNames);

	const float frac =
		(job->total > 0) ? static_cast<float>(job->done) / static_cast<float>(job->total) : 0.f;
	ImGui::ProgressBar(frac, ImVec2(-1.f, 0.f));
	ImGui::Text("Status: %s   %d / %d hosts   %d alive", job->status.c_str(), job->done, job->total,
				job->aliveCount);
	if (!scanPack->lastError().empty() && job->status != "scanning") {
		ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f), "%s", scanPack->lastError().c_str());
	}

	ImGui::Separator();

	std::vector<HostRow> rows;
	for (auto e : ecs->view<ecs::CNetHost>()) {
		auto& h = ecs->getComponent<ecs::CNetHost>(e);
		if (m_aliveOnly && !h.alive) {
			continue;
		}
		HostRow row;
		row.ip = h.ip;
		row.hostname = h.hostname;
		row.alive = h.alive;
		row.rttMs = h.rttMs;
		row.openPorts = h.openPorts;
		row.url = netscan::webUrl(h.ip, h.openPorts);
		if (!netscan::parseIpv4(h.ip, row.ipn)) {
			row.ipn = 0;
		}
		rows.push_back(std::move(row));
	}
	std::sort(rows.begin(), rows.end(),
			  [](const HostRow& a, const HostRow& b) { return a.ipn < b.ipn; });

	// No Resizable: ScrollY + stretch columns + drag-resize asserts / AV in imgui_tables.
	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
								  ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp |
								  ImGuiTableFlags_NoSavedSettings;
	const float tableH =
		(std::max)(ImGui::GetContentRegionAvail().y, ImGui::GetFrameHeight() * 4.f);
	if (ImGui::BeginTable("nethosts", 6, flags, ImVec2(0.f, tableH))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("IP", ImGuiTableColumnFlags_WidthFixed, ipW);
		ImGui::TableSetupColumn("Hostname");
		ImGui::TableSetupColumn("Alive", ImGuiTableColumnFlags_WidthFixed, 60.f);
		ImGui::TableSetupColumn("RTT ms", ImGuiTableColumnFlags_WidthFixed, 70.f);
		ImGui::TableSetupColumn("Open ports");
		ImGui::TableSetupColumn("URL");
		ImGui::TableHeadersRow();
		for (const auto& row : rows) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(row.ip.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(row.hostname.empty() ? "-" : row.hostname.c_str());
			ImGui::TableSetColumnIndex(2);
			if (row.alive) {
				ImGui::TextUnformatted("yes");
			} else {
				ImGui::TextDisabled("no");
			}
			ImGui::TableSetColumnIndex(3);
			if (row.rttMs >= 0) {
				ImGui::Text("%d", row.rttMs);
			} else {
				ImGui::TextDisabled("-");
			}
			ImGui::TableSetColumnIndex(4);
			ImGui::TextUnformatted(row.openPorts.empty() ? "-" : row.openPorts.c_str());
			ImGui::TableSetColumnIndex(5);
			if (row.url.empty()) {
				ImGui::TextDisabled("-");
			} else {
				ImGui::PushID(row.ip.c_str());
				ImGui::TextLinkOpenURL(row.url.c_str());
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
}

} // namespace rigkit
