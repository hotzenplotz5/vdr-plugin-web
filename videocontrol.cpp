#include <chrono>
#include <thread>
#include "BrowserClient.h"
#include "videocontrol.h"

bool isPlayerActivated;

static uchar buf[188], bufsize;

VideoPlayer::VideoPlayer() {
    dsyslog("[vdrweb] Create Player...");
    pause = false;
    bufsize = 0;
    tsPlayed = false;
}

VideoPlayer::~VideoPlayer() {
    dsyslog("[vdrweb] Delete Player...");

    pause = true;
    setVideoFullscreen();

    Detach();
}

void VideoPlayer::Activate(bool On) {
    dsyslog("[vdrweb] Activate video player: %s", On ? " Ja" : "Nein");

    if (On) {
        isPlayerActivated = true;
    } else {
        isPlayerActivated = false;
        setVideoFullscreen();
    }
}

void VideoPlayer::Pause() {
    pause = true;
    DeviceFreeze();
}

void VideoPlayer::Resume() {
    // Only clear the decoder when resuming from an actual pause. Some HbbTV
    // applications send ResumeVideo while playback is already running.
    if (pause) {
        ResetVideo();
        bufsize = 0;
    }

    DevicePlay();
    pause = false;
}

void VideoPlayer::setVideoFullscreen() {
    // fullscreen
    cDevice::PrimaryDevice()->ScaleVideo(cRect::Null);
}

void VideoPlayer::ResetVideo() {
    if (tsPlayed) {
        dsyslog("VideoPlayer::ResetVideo");
        DeviceClear();
        DevicePlay();
        tsPlayed = false;
    }

    bufsize = 0;
}

void VideoPlayer::SetVideoSize(int x, int y, int width, int height) {
    dsyslog("[vdrweb] SetVideoSize in video player: x=%d, y=%d, width=%d, height=%d", x, y, width, height);

    int newX, newY, newWidth, newHeight;
    calcVideoPosition(x, y, width,height, &newX, &newY, &newWidth, &newHeight);

    dsyslog("[vdrweb] SetVideoSize calculated: x=%d, y=%d, width=%d, height=%d", newX, newY, newWidth, newHeight);

    cRect r = {newX, newY, newWidth, newHeight};
    cRect availableRect = cDevice::PrimaryDevice()->CanScaleVideo(r);
    cDevice::PrimaryDevice()->ScaleVideo(availableRect);
}

void VideoPlayer::calcVideoPosition(int x, int y, int w, int h, int *newx, int *newy, int *newwidth, int *newheight) {
    int osdWidth;
    int osdHeight;
    double osdPh;
    cDevice::PrimaryDevice()->GetOsdSize(osdWidth, osdHeight, osdPh);

    *newx = (x * osdWidth) / 1280;
    *newy = (y * osdHeight) / 720;
    *newwidth = (w * osdWidth) / 1280;
    *newheight = (h * osdHeight) / 720;
}

void VideoPlayer::PlayPacket(uint8_t *buffer, int len) {
    tsError = false;

    // if player is paused, discard all incoming packets
    if (pause) {
        return;
    }

    if (len <= 0) {
        return;
    }

    // play saved partial packet
    if (bufsize) {
        const int missing = 188 - bufsize;
        if (len < missing) {
            memcpy(buf + bufsize, buffer, len);
            bufsize += len;
            return;
        }

        memcpy(buf + bufsize, buffer, missing);
        if (!PlayData(buf, 188)) {
            bufsize = 0;
            return;
        }

        buffer += missing;
        len -= missing;
        bufsize = 0;
    }

    // save partial packet
    const int rest = len % 188;
    if (rest) {
        memcpy(buf, buffer + len - rest, rest);
        len -= rest;
        bufsize = rest;
    }

    PlayData(buffer, len);
}

bool VideoPlayer::PlayData(uint8_t *data, int len) {
    // The device buffer may be full. Retry quickly for up to five seconds
    // without progress instead of sleeping 500 ms per attempt.
    int waited = 0;

    while (len >= 188) {
        if (videoPlayer == nullptr) {
            return false;
        }

        const int result = PlayTs(data, len);

        if (result < 0) {
            tsError = true;
            return false;
        }

        if (result == 0) {
            if (waited >= 5000) {
                tsError = true;
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            waited += 5;
            continue;
        }

        len -= result;
        data += result;
        waited = 0;
        tsPlayed = true;
    }

    return true;
}

void VideoPlayer::SelectAudioTrack(std::string track) {
    // TODO: Wie wechselt man den Audiotrack?
    // printf("Anzahl der Audio-Tracks: %d\n", cDevice::PrimaryDevice()->NumAudioTracks());
}
