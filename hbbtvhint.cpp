/*
 * hbbtvhint.cpp: shows "HbbTV available" after a channel switch
 */

#include <vdr/player.h>
#include <vdr/i18n.h>
#include "hbbtvhint.h"
#include "webosdpage.h"

#define HBBTVHINT_OSD_LEVEL  (OSD_LEVEL_SUBTITLES + 1)
#define HBBTVHINT_PENDING_MS 15000

cHbbtvHintSetup hbbtvHintSetup;

cMutex cHbbtvHint::mutex;
bool cHbbtvHint::pending = false;
bool cHbbtvHint::shown = false;
bool cHbbtvHint::hideRequest = false;
cTimeMs cHbbtvHint::pendingTimeout;
cTimeMs cHbbtvHint::showTimeout;
cOsd *cHbbtvHint::osd = nullptr;

void cHbbtvHint::ChannelSwitch() {
    cMutexLock lock(&mutex);
    pending = false;
    shown = false;
    hideRequest = true;
}

void cHbbtvHint::AutostartApplicationFound() {
    cMutexLock lock(&mutex);

    if (!hbbtvHintSetup.enabled || shown || pending)
        return;

    pending = true;
    pendingTimeout.Set(HBBTVHINT_PENDING_MS);
}

void cHbbtvHint::Hide() {
    cMutexLock lock(&mutex);
    pending = false;
    hideRequest = true;
}

void cHbbtvHint::Close() {
    if (osd != nullptr) {
        delete osd;
        osd = nullptr;
    }
}

static bool screenBusy() {
    if (cOsd::IsOpen() || WebOSDPage::IsOpen())
        return true;

    cMutexLock controlLock;
    return cControl::Control(controlLock, false) != nullptr;
}

void cHbbtvHint::Draw() {
    const cFont *font = cFont::GetFont(fontOsd);
    const char *text = tr("HbbTV available");

    const int lineHeight = font->Height();
    const int padding = lineHeight / 2;
    const int width = padding + font->Width(text) + padding;
    const int height = lineHeight + padding;
    const int margin = lineHeight;

    const int left = cOsd::OsdLeft() + margin;
    const int top = cOsd::OsdTop() + margin;
    const int right =
        cOsd::OsdLeft() + cOsd::OsdWidth() - margin - width;
    const int bottom =
        cOsd::OsdTop() + cOsd::OsdHeight() - margin - height;

    const int x =
        (hbbtvHintSetup.position == 0 ||
         hbbtvHintSetup.position == 2) ? right : left;
    const int y =
        (hbbtvHintSetup.position <= 1) ? bottom : top;

    osd = cOsdProvider::NewOsd(x, y, HBBTVHINT_OSD_LEVEL);
    if (osd == nullptr)
        return;

    tArea area = { 0, 0, width - 1, height - 1, 32 };
    if (osd->CanHandleAreas(&area, 1) != oeOk)
        area.bpp = 8;

    if (osd->SetAreas(&area, 1) != oeOk) {
        esyslog("[vdrweb] HbbTV hint: unable to set OSD area");
        Close();
        return;
    }

    const tColor clrBack = 0xD0202020;
    const int radius = height / 2;

    osd->DrawRectangle(
        0, 0, width - 1, height - 1, clrTransparent);
    osd->DrawRectangle(
        radius, 0, width - 1 - radius, height - 1, clrBack);
    osd->DrawEllipse(
        0, 0, radius - 1, height - 1, clrBack, 7);
    osd->DrawEllipse(
        width - radius, 0, width - 1, height - 1, clrBack, 5);
    osd->DrawText(
        padding, padding / 2, text,
        clrWhite, clrBack, font,
        font->Width(text), lineHeight, taLeft);
    osd->Flush();
}

void cHbbtvHint::Process() {
    cMutexLock lock(&mutex);

    if (osd != nullptr &&
        (hideRequest ||
         !hbbtvHintSetup.enabled ||
         showTimeout.TimedOut() ||
         screenBusy())) {
        Close();
    }

    hideRequest = false;

    if (!pending)
        return;

    if (!hbbtvHintSetup.enabled || pendingTimeout.TimedOut()) {
        pending = false;
        return;
    }

    if (osd == nullptr && !screenBusy()) {
        pending = false;
        shown = true;
        Draw();
        showTimeout.Set(hbbtvHintSetup.duration * 1000);
    }
}
