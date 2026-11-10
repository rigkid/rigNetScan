#pragma once

#include "ImGuiPanel.h"
namespace rigkit {

/**
 * @brief LAN TCP scan table over `CNetScan` / `CNetHost`.
 * @details Register once after **rigImGui** and **rigNetScan** setup:
 * `wm->createWindow<IpScannerWindow>()`. Title is `kTitle`. Looks up
 * `rigNetScan` on the engine - do not pass the pack in.
 */
class IpScannerWindow : public ImGuiPanel {
  public:
	static constexpr const char* kTitle = "IP Scanner";

	IpScannerWindow();

  protected:
	void renderContents() override;

  private:
	bool m_aliveOnly = true;
};

} // namespace rigkit
