/*
 * hbbtvhint.h: small "HbbTV available" OSD hint
 */

#pragma once

#include <vdr/osd.h>
#include <vdr/thread.h>
#include <vdr/tools.h>

struct cHbbtvHintSetup {
    int enabled = 1;
    int duration = 7;
    int position = 0; // 0 bottom right, 1 bottom left, 2 top right, 3 top left
};

extern cHbbtvHintSetup hbbtvHintSetup;

class cHbbtvHint {
private:
    static cMutex mutex;
    static bool pending;
    static bool shown;
    static bool hideRequest;
    static cTimeMs pendingTimeout;
    static cTimeMs showTimeout;
    static cOsd *osd;

    static void Draw();
    static void Close();

public:
    static void ChannelSwitch();
    static void AutostartApplicationFound();
    static void Hide();
    static void Process();
};
