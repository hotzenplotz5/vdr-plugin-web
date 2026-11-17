#include <vdr/device.h>
#include <vdr/remote.h>
#include <Magick++/Blob.h>
#include <Magick++/Image.h>
#include <chrono>
#include <cstdint>
#include <mutex>
#include "webosdpage.h"
#include "web.h"
#include "service/vdrsuite_hbbtv_presentation_service.h"

// #define MEASURE_SCALE_TIME 1
// #define DEBUG_SAVE_OSD_IMAGE 1

#include "qoi.h"

std::recursive_mutex pixmap_mutex;

WebOSDPage *webOsdPage;
WebOSDPage *playerWebOsdPage;

OSD_MODE currentOsdMode = CLOSED;

// initialize keyMap
std::map<int, std::string> keyMap({
        {kUp, "VK_UP"},
        {kDown, "VK_DOWN"},
        {kLeft, "VK_LEFT"},
        {kRight, "VK_RIGHT"},
        {kOk, "VK_ENTER"},
        {kBack, "VK_BACK"},
        {kRed, "VK_RED"},
        {kGreen, "VK_GREEN"},
        {kYellow, "VK_YELLOW"},
        {kBlue, "VK_BLUE"},
        {k0, "VK_0"},
        {k1, "VK_1"},
        {k2, "VK_2"},
        {k3, "VK_3"},
        {k4, "VK_4"},
        {k5, "VK_5"},
        {k6, "VK_6"},
        {k7, "VK_7"},
        {k8, "VK_8"},
        {k9, "VK_9"},
        {kPlay, "VK_PLAY"},
        {kPause, "VK_PAUSE"},
        {kStop, "VK_STOP"},
        {kFastFwd, "VK_FAST_FWD"},
        {kFastRew, "VK_REWIND"},
        {kPlayPause, "VK_PLAY_PAUSE"},
        {kRecord, "VK_RECORD"},
        {kNext, "VK_TRACK_NEXT"},
        {kPrev, "VK_TRACK_PREV"},
        {kInfo, "VK_INFO"},
        {kSubtitles, "VK_SUBTITLE"},
});

WebOSDPage *WebOSDPage::Create(bool useOutputDeviceScale, OSD_MODE osdMode) {
    switch(osdMode) {
        case OSD:
            if (webOsdPage == nullptr) {
                webOsdPage = new WebOSDPage(useOutputDeviceScale, osdMode);
            }
            webOsdPage->useOutputDeviceScale = useOutputDeviceScale;
            currentOsdMode = OSD;
            return webOsdPage;

        case PLAYER:
            if (playerWebOsdPage == nullptr) {
                playerWebOsdPage = new WebOSDPage(useOutputDeviceScale, osdMode);
            }
            playerWebOsdPage->useOutputDeviceScale = useOutputDeviceScale;
            currentOsdMode = PLAYER;
            return playerWebOsdPage;

        case CLOSED:
            return nullptr;
    }

    return nullptr;
}

WebOSDPage *WebOSDPage::Get() {
    if (currentOsdMode == OSD) {
        // dsyslog("[vdrweb] WebOSDPage::Get(%d): Return webOsdPage %p", (int)currentOsdMode, webOsdPage);
        return webOsdPage;
    } else if (currentOsdMode == PLAYER) {
        // dsyslog("[vdrweb] WebOSDPage::Get(%d): Return playerWebOsdPage %p", (int)currentOsdMode, playerWebOsdPage);
        return playerWebOsdPage;
    }

    // dsyslog("[vdrweb] WebOSDPage::Get(-1): Return nullptr");
    return nullptr;
}

bool WebOSDPage::IsOpen() {
    return (currentOsdMode == OSD && webOsdPage != nullptr)
       || (currentOsdMode == PLAYER && playerWebOsdPage != nullptr);
}

WebOSDPage::WebOSDPage(bool useOutputDeviceScale, OSD_MODE osdMode)
        : cControl(nullptr), useOutputDeviceScale(useOutputDeviceScale)
{
    dsyslog("[vdrweb] Create WebOSDPage, osdMode %d", (int)osdMode);

    osd = nullptr;
    pixmap = nullptr;
    disp_height = 1920;
    disp_width = 1080;
    lastVolume = -1;
    lastVolumeTime = time(NULL);

    currentMode = osdMode;
}

WebOSDPage::~WebOSDPage() {
    dsyslog("[vdrweb] Destruct WebOSDPage, osdMode %d", (int)currentMode);

    std::lock_guard<std::recursive_mutex> lock(pixmap_mutex);
    if (osd != nullptr) {
        if (pixmap != nullptr) {
            osd->DestroyPixmap(pixmap);
        }
        delete osd;
        osd = nullptr;
    }

    pixmap = nullptr;

    switch (currentMode) {
        case OSD:
            webOsdPage = nullptr;
            break;

        case PLAYER:
            playerWebOsdPage = nullptr;
            break;

        case CLOSED:
            break;
    }

    // cDevice::PrimaryDevice()->ScaleVideo(cRect::Null);
}

void WebOSDPage::Show() {
    dsyslog("[vdrweb] WebOSDPage show\n");

    Display();
}

void WebOSDPage::Display() {
    dsyslog("[vdrweb] WebOSDPage Display\n");
    if (osd != nullptr) {
        std::lock_guard<std::recursive_mutex> lock(pixmap_mutex);

        if (pixmap != nullptr) {
            osd->DestroyPixmap(pixmap);
            pixmap = nullptr;
        }

        delete osd;
        osd = nullptr;
    }

    osd = cOsdProvider::NewOsd(0, 0, 5);

    double ph;
    cDevice::PrimaryDevice()->GetOsdSize(disp_width, disp_height, ph);
    tArea area {0, 0, disp_width - 1, disp_height - 1, 32};
    auto areaResult = osd->SetAreas(&area, 1);

    if (areaResult == oeOk) {
        isyslog("[vdrweb] Area size set to %d:%d - %d:%d", 0, 0, disp_width - 1, disp_height - 1);
    }

    SetOsdSize();

    browserClient->ReloadOSD();
}

void WebOSDPage::SetOsdSize() {
    dsyslog("[vdrweb] webospage SetOsdSize()");

    std::lock_guard<std::recursive_mutex> lock(pixmap_mutex);
    if (pixmap != nullptr) {
        dsyslog("[vdrweb] webosdpage SetOsdSize, Destroy old pixmap");

        osd->DestroyPixmap(pixmap);
        pixmap = nullptr;
    }

    dsyslog("[vdrweb] webosdpage SetOsdSize, Get new OSD size");
    double ph;
    cDevice::PrimaryDevice()->GetOsdSize(disp_width, disp_height, ph);

    /*
    if (disp_width <= 0 || disp_height <= 0 || disp_width > 3840 || disp_height > 2160) {
        esyslog("[vdrweb] Got illegal OSD size %dx%d", disp_width, disp_height);
        return;
    }
    */

    cRect rect(0, 0, disp_width, disp_height);

    // try to get a pixmap
    dsyslog("[vdrweb] webosdpage SetOsdSize, Create pixmap %dx%d", disp_width, disp_height);
    pixmap = osd->CreatePixmap(5, rect, rect);

    dsyslog("[vdrweb] webosdpage SetOsdSize, Clear Pixmap");
    LOCK_PIXMAPS;
    pixmap->Clear();
}

bool WebOSDPage::drawImage(uint8_t* image, int render_width, int render_height, int x, int y, int width, int height) {

    if (!VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
            image,
            render_width,
            render_height,
            x,
            y,
            width,
            height)) {
        esyslog("[vdrweb] failed to capture HbbTV presentation patch");
        return false;
    }

#ifdef DEBUG_SAVE_OSD_IMAGE
    static int osd_image_number = 0;

    // the qoi encoder expects the RGBA pixel format.
    // For debugging purposes make a copy of the buffer and change the pixel format
    uint32_t* image_copy = new uint32_t[width * height];
    uint32_t* image_u32 = (uint32_t*) image;

    for (int i = 0; i < width * height; ++i) {
        // Source: BGRA = 0xAARRGGBB
        // Dest:   RGBA = 0xAABBGGRR
        image_copy[i] =
                ((image_u32[i] & 0xFF00FF00)      ) | // AA__GG__
                ((image_u32[i] & 0x00FF0000) >> 16) | // __RR____ -> ______RR
                ((image_u32[i] & 0x000000FF) << 16);  // ______BB -> __BB____
    }

    std::string filename = std::string("/tmp/osd_image_") + std::to_string(osd_image_number) + ".qoi";

    qoi_desc desc {
        .width = static_cast<unsigned int>(width),
        .height = static_cast<unsigned int>(height),
        .channels = 4,
        .colorspace = QOI_LINEAR
    };
    qoi_write(filename.c_str(), image_copy, &desc);
    osd_image_number++;

    delete(image_copy);
#endif

    return scaleAndPaint(image, render_width, render_height, x, y, width, height);
}

bool WebOSDPage::drawFrame(
        const pluginweb::ProcessOsdFrameType &frame) {
    bool ok = true;

    for (const auto &r : frame.rects) {
        if (frame.qoi) {
            if (!VdrSuiteHbbtvPresentationStore::ApplyQoiPatch(
                    r.data,
                    frame.render_width,
                    frame.render_height,
                    r.x,
                    r.y)) {
                esyslog("[vdrweb] failed to capture HbbTV QOI frame region");
                ok = false;
                continue;
            }

            qoi_desc desc;
            void *image = qoi_decode(
                r.data.c_str(),
                static_cast<int>(r.data.size()),
                &desc,
                4);
            if (image == nullptr) {
                esyslog("[vdrweb] failed to decode qoi OSD frame region");
                ok = false;
                continue;
            }

            ok &= scaleAndPaint(
                static_cast<uint8_t *>(image),
                frame.render_width,
                frame.render_height,
                r.x,
                r.y,
                static_cast<int>(desc.width),
                static_cast<int>(desc.height),
                false);
            free(image);
        } else {
            const std::int64_t expected =
                static_cast<std::int64_t>(r.width) *
                static_cast<std::int64_t>(r.height) * 4;
            if (r.width <= 0 ||
                r.height <= 0 ||
                expected != static_cast<std::int64_t>(r.data.size())) {
                esyslog("[vdrweb] invalid raw OSD frame region");
                ok = false;
                continue;
            }

            auto *pixels = reinterpret_cast<uint8_t *>(
                const_cast<char *>(r.data.data()));

            if (!VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
                    pixels,
                    frame.render_width,
                    frame.render_height,
                    r.x,
                    r.y,
                    r.width,
                    r.height)) {
                esyslog("[vdrweb] failed to capture HbbTV frame region");
                ok = false;
                continue;
            }

            ok &= scaleAndPaint(
                pixels,
                frame.render_width,
                frame.render_height,
                r.x,
                r.y,
                r.width,
                r.height,
                false);
        }
    }

    // Compose the complete frame before presenting it.
    std::lock_guard<std::recursive_mutex> lock(pixmap_mutex);
    if (osd != nullptr)
        osd->Flush();

    return ok;
}

bool WebOSDPage::drawImageQOI(const std::string qoibuffer, const int render_width, const int render_height, const int x, const int y) {
    // decode image data
    qoi_desc desc;
    void *image = qoi_decode(qoibuffer.c_str(), (int)qoibuffer.size(), &desc, 4);

    if (image == nullptr) {
        // something failed
        esyslog("[vdrweb] failed to decode qoi OSD image");
        return false;
    }

    if (!VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
            static_cast<uint8_t *>(image),
            render_width,
            render_height,
            x,
            y,
            static_cast<int>(desc.width),
            static_cast<int>(desc.height))) {
        free(image);
        esyslog("[vdrweb] failed to capture HbbTV QOI presentation patch");
        return false;
    }

    bool retValue = scaleAndPaint(static_cast<uint8_t *>(image), render_width, render_height, x, y, (int)desc.width, (int)desc.height);
    free(image);

    return retValue;
}

bool WebOSDPage::scaleAndPaint(uint8_t* image, int render_width, int render_height,
                               int x, int y, int width, int height,
                               bool flush) {
    // sanity check
    if (width > 3840 || height > 2160 || width <= 0 || height <= 0) {
        return false;
    }

    bool scaleRequired = (disp_width != render_width) || (disp_height != render_height);

    // dsyslog("[vdrweb] RenderSize: %d x %d, DisplaySize: %d x %d, x=%d,y=%d, Scaling required: %s", render_width, render_height, disp_width, disp_height, x, y, (scaleRequired ? "yes" : "no"));

    if (!scaleRequired) {
        // dsyslog("[vdrweb] no scaling required");

        // scaling is not needed. Draw the image as is.
        const cImage recImage(cSize(width, height), (const tColor *)image);

        std::lock_guard<std::recursive_mutex> lock(pixmap_mutex);
        if (pixmap != nullptr) {
            LOCK_PIXMAPS;
            pixmap->DrawImage(cPoint(x,y), recImage);
        } else {
            esyslog("[vdrweb] Pixmap is null. OSD not available");
        }

        if (flush && osd != nullptr) {
            osd->Flush();
        }

        return true;
    }

    // calculate scale factor
    double scalex = disp_width / (double)render_width;
    double scaley = disp_height / (double)render_height;

    // calculate coordinates
    int osd_x = (int)lround(scalex * x);
    int osd_y = (int)lround(scaley * y);
    int osd_width = (int)lround(scalex * width);
    int osd_height = (int)lround(scaley * height);

    // dsyslog("[vdrweb] Scaling: scalex=%2f, scaley=%2f, osd_x=%d, osd_y=%d", scalex, scaley, osd_x, osd_y);

#ifdef ENABLE_FAST_SCALE
    if (useOutputDeviceScale) {
        // dsyslog("[vdrweb] scaling using outputdevice");

        const cImage recImage(cSize(width, height), (const tColor *)image);

        std::lock_guard<std::recursive_mutex> lock(pixmap_mutex);
        if (pixmap != nullptr) {
            LOCK_PIXMAPS;
            pixmap->DrawScaledImage(cPoint(x * scalex, y * scaley), recImage, scalex, scaley, true);
        } else {
            esyslog("[vdrweb] Pixmap is null. OSD not available");
        }
    } else {
#endif // ENABLE_FAST_SCALE
        // dsyslog("[vdrweb] scaling using GraphicsMagick");

        // create image buffer for scaled image
        cSize recImageSize(osd_width, osd_height);
        const cImage recImage(recImageSize);
        auto *scaled = (uint8_t *) (recImage.Data());

        if (scaled == nullptr) {
            esyslog("[vdrweb] Out of memory reading OSD image");
            return true;
        }

#ifdef MEASURE_SCALE_TIME
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
#endif

        Magick::Image mimage;
        mimage.read(width, height, "RGBA", static_cast<const MagickLib::StorageType>(0), image);
        mimage.sample(Magick::Geometry(osd_width, osd_height));
        mimage.write ( 0, 0, osd_width, osd_height, "RGBA", static_cast<const MagickLib::StorageType>(0), scaled);

#ifdef MEASURE_SCALE_TIME
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    dsyslog("[vdrweb] Scale time = %lu [ms]", std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
#endif

        std::lock_guard<std::recursive_mutex> lock(pixmap_mutex);
        if (pixmap != nullptr) {
            LOCK_PIXMAPS;
            pixmap->DrawImage(cPoint(x * scalex, y * scaley), recImage);
        } else {
            esyslog("[vdrweb] Pixmap is null. OSD not available");
        }
#ifdef ENABLE_FAST_SCALE
    }
#endif

    if (flush && osd != nullptr) {
        osd->Flush();
    }

    return true;
}


eOSState WebOSDPage::ProcessKey(eKeys Key) {
    // check if volumebar is displayed and kill it after a timeout
    // a volume change resets the timeout
    if (pixmapVol && (time(NULL) - lastVolumeTime > 3))
        DeleteVolume();

    if (Key & k_Release)
        return osContinue;

    // Repeated keys (for example held cursor keys) use the normal key code.
    const eKeys key = NORMALKEY(Key);

    auto search = ::keyMap.find(key);
    if (search != ::keyMap.end() &&
        browserClient != nullptr &&
        browserClient->ProcessKey(search->second)) {
        return osContinue;
    }

    // If the HbbTV application does not consume a channel/numeric key, hand it
    // back to VDR like a normal TV instead of swallowing it in the web OSD.
    const bool isChannelKey =
        (key >= k0 && key <= k9) ||
        key == kChanUp ||
        key == kChanDn ||
        key == kChanPrev;

    if (isChannelKey && !(Key & k_Repeat)) {
        dsyslog("[vdrweb] key %d not used by application, pass to VDR",
                static_cast<int>(key));
        cRemote::Put(key);
        return osEnd;
    }

    return osContinue;
}

void WebOSDPage::Hide() {
    dsyslog("[vdrweb] WebOSDPage::Hide...");
}

void WebOSDPage::DrawVolume(int volume) {
    dsyslog("[vdrweb] WebOSDPage::DrawVolume...");
   if (volume != lastVolume) {
      int x = 0;           // volumebar x
      int y = 20;          // volumebar y
      int w = disp_width;  // volumebar width
      int h = 20;          // volumebar height
      if (!pixmapVol) {
          pixmapVol = osd->CreatePixmap(7, cRect(x, y, w, h));
          pixmapVol->Fill(clrTransparent);
      }
      if (volume) {
         int p = (w - 1) * volume / MAXVOLUME;
         pixmapVol->DrawRectangle(cRect(0, 0, p - 1, h - 1), clrVolumeBarLower);
         pixmapVol->DrawRectangle(cRect(p, 0, w - 1, h - 1), clrVolumeBarUpper);
      } else {
         pixmapVol->DrawRectangle(cRect(0, 0, w - 1, h - 1), clrVolumeBarUpper);
      }
      lastVolume = volume;
      // (re)set the display volumebar timeout
      lastVolumeTime = time(NULL);
      if (osd != nullptr)
         osd->Flush();
   }
}

void WebOSDPage::DeleteVolume(void) {
    dsyslog("[vdrweb] WebOSDPage::DeleteVolume...: %s", pixmapVol != nullptr ? "pixmapVol != null" : "pixmapVol == null");
    if (pixmapVol) {
        osd->DestroyPixmap(pixmapVol);
        osd->Flush();
        pixmapVol = nullptr;
    }
}

cOsdObject *WebOSDPage::GetInfo() {
    dsyslog("[vdrweb] WebOSDPage::GetInfo...");

    // VDR deletes the object returned by GetInfo() when the info menu closes.
    // Returning this would delete the running player control.
    return nullptr;
}
