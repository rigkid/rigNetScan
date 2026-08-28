#include "app.h"

#include "core/pack/MPack.h"
#include "core/RigKitEngine.h"
#include "rendering/U_gladGlfw.h"
#include "packs/rigComponent/src/rigComponent.h"
#include "packs/rigImGui/src/Mui.h"
#include "packs/rigImGui/src/rigImGui.h"
#include "packs/rigNetScan/src/IpScannerWindow.h"
#include "packs/rigNetScan/src/rigNetScan.h"
#include "imgui_internal.h"

#include <cstdlib>
#include <iostream>
#include <spdlog/spdlog.h>

NetscanApp::NetscanApp() {
	window().width = 1100;
	window().height = 700;
	window().title = "RigKit - netscan";
	settings().appName = "netscan";
}

void NetscanApp::parseCommandLineArgs(const rigkit::CommandLineArgs& args) {
	rigkit::IApp::parseCommandLineArgs(args);
	m_cliArgs = args;
	if (args.hasFlag("smoke-netscan")) {
		m_smoke = true;
	}
	if (args.hasFlag("help") || args.hasFlag("h")) {
		std::cout << "netscan - LAN TCP connect scanner (rigNetScan)\n\n"
				  << "  --smoke-netscan   Listen on loopback, scan 127.0.0.1, exit\n"
				  << "  --help\n";
		std::exit(0);
	}
}

void NetscanApp::bootstrapPacks() {
	auto* packs = getEngine()->getPackManager();
	if (!packs) {
		return;
	}
	packs->registerPack<rigkit::rigComponent>();
	packs->registerPack<rigkit::rigImGui>();
	packs->registerPack<rigkit::rigNetScan>();
	packs->initAll();
	packs->setupAll();
	m_scan = packs->getPack<rigkit::rigNetScan>();
}

void NetscanApp::setupUi() {
	auto* ui = getEngine()->getUiManager();
	if (!ui) {
		spdlog::warn("netscan: no IMui - rigImGui failed to attach");
		return;
	}
	auto* mui = dynamic_cast<rigkit::Mui*>(ui);
	if (!mui) {
		return;
	}
	mui->setDockPassthroughCentral(false);
	mui->addHostPanel(rigkit::HostPanel::Windows);
	mui->addHostPanel(rigkit::HostPanel::Log);

	auto* wm = mui->getWindowManager();
	if (wm) {
		wm->createWindow<rigkit::IpScannerWindow>();
	}

	mui->setDockLayoutBuilder([mui](ImGuiID dockspaceId) {
		ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
		if (!node) {
			return;
		}
		if (node->IsSplitNode()) {
			mui->setDockLayoutBuilder(nullptr);
			return;
		}
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::DockBuilderRemoveNode(dockspaceId);
		ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspaceId, vp->WorkSize);

		ImGuiID center = 0, bottom = 0;
		ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Down, 0.22f, &bottom, &center);

		ImGui::DockBuilderDockWindow("Log", bottom);
		ImGui::DockBuilderDockWindow(rigkit::IpScannerWindow::kTitle, center);
		ImGui::DockBuilderFinish(dockspaceId);
		mui->setDockLayoutBuilder(nullptr);
	});

	if (wm) {
		wm->showWindow(rigkit::IpScannerWindow::kTitle);
		wm->showWindow("Log");
		wm->hideWindow("Windows");
	}
	spdlog::info("netscan UI: IP Scanner table");
}

bool NetscanApp::runSmoke() {
	if (!m_scan) {
		spdlog::error("smoke-netscan: rigNetScan not registered");
		return false;
	}
	return m_scan->smokeLoopback();
}

void NetscanApp::setup() {
	spdlog::info("netscan setup");
	m_engine->setClearColor(0.10f, 0.11f, 0.13f, 1.0f);
	bootstrapPacks();
	if (!m_smoke) {
		setupUi();
	}
	if (m_smoke) {
		const bool ok = runSmoke();
		if (!ok) {
			spdlog::error("netscan --smoke-netscan failed: {}",
						  m_scan ? m_scan->lastError() : "no pack");
		} else {
			spdlog::info("netscan --smoke-netscan OK");
		}
		if (auto* win = getEngine()->getWindow()) {
			glfwSetWindowShouldClose(win, GLFW_TRUE);
		}
		if (!ok) {
			std::exit(1);
		}
	}
}

void NetscanApp::update(float) {}
