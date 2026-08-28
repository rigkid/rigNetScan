#pragma once

#include <memory>

#include "core/IApp.h"
#include "core/util/CommandLineArgs.h"

namespace rigkit {
class rigNetScan;
}

class NetscanApp : public rigkit::IApp {
  public:
	NetscanApp();

	void parseCommandLineArgs(const rigkit::CommandLineArgs& args) override;

  protected:
	void setup() override;
	void update(float dt) override;
	void draw() override {}
	void exit() override {}

  private:
	void bootstrapPacks();
	void setupUi();
	bool runSmoke();

	bool m_smoke = false;
	rigkit::CommandLineArgs m_cliArgs;
	std::shared_ptr<rigkit::rigNetScan> m_scan;
};
