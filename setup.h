/*
 * setup.h: technical setup of the web plugin
 */

#pragma once

#include <vdr/menuitems.h>

struct cWebSetup {
    int resolution = 0;    // 0 browser default, 1..4 = 1280..3840
    int deviceScale = -1;  // -1 follows command-line -f
    int frameRate = 0;     // 0 browser default, 1..4 = 25/30/50/60
    int mse = 1;
    int zdfQuirks = 1;

    int Width() const;
    int FrameRate() const;
};

extern cWebSetup webSetup;

// Push current render/runtime options to the browser.
void SendRenderParameters();

class cMenuSetupWeb : public cMenuSetupPage {
private:
    cWebSetup web;
    const char *resolutions[5];
    const char *frameRates[5];

protected:
    void Store() override;

public:
    cMenuSetupWeb();
};
