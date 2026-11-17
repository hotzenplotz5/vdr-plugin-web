#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include "BrowserClient.h"
#include "videocontrol.h"
#include <vdr/remux.h>

bool isPlayerActivated;

static uchar buf[188], bufsize;

VideoPlayer::VideoPlayer() {
    dsyslog("[vdrweb] Create Player...");
    pause = false;
    mse = false;
    bufsize = 0;
    tsPlayed = false;
    ResetStatus();
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
    if (pause && !mse) {
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

void VideoPlayer::ResetStatus() {
    firstPts = -1;
    lastPts = -1;
    endOfStream = false;
    lastStc = -1;
    stcStall.Set();
    lastData.Set();
}

void VideoPlayer::SetEndOfStream() {
    dsyslog("[vdrweb] end of stream");
    endOfStream = true;
}

// Remember the first and last video PTS carried by incoming TS packets.
void VideoPlayer::ScanPts(const uint8_t *data, int len) {
    for (; len >= TS_SIZE; data += TS_SIZE, len -= TS_SIZE) {
        if (data[0] != TS_SYNC_BYTE ||
            !TsHasPayload(data) ||
            !TsPayloadStart(data))
            continue;

        const uchar *payload = data;
        const int length = TsGetPayload(&payload);

        if (length >= 14 &&
            payload[0] == 0 &&
            payload[1] == 0 &&
            payload[2] == 1 &&
            (payload[3] & 0xF0) == 0xE0 &&
            PesHasPts(payload)) {
            lastPts = PesGetPts(payload);
            if (firstPts < 0)
                firstPts = lastPts;
        }
    }

    lastData.Set();
}

void VideoPlayer::GetStatus(std::int64_t &position, int &state) {
    const std::int64_t stc = static_cast<std::int64_t>(DeviceGetSTC());
    const bool stcValid =
        stc >= 0 &&
        stc != static_cast<std::int64_t>(-1) &&
        firstPts >= 0;

    if (stcValid && stc != lastStc) {
        lastStc = stc;
        stcStall.Set();
    }

    position = -1;
    if (stcValid) {
        const std::int64_t diff = PtsDiff(firstPts, stc);
        if (diff >= 0 && diff < 24LL * 3600 * PTSTICKS)
            position = diff / (PTSTICKS / 1000);
    }

    if (pause) {
        state = STATE_PAUSED;
    } else if (endOfStream &&
               (stcValid ? stcStall.Elapsed() >= 1000
                         : lastData.Elapsed() >= 3000)) {
        state = STATE_ENDED;
    } else if (!endOfStream &&
               stcValid &&
               stcStall.Elapsed() >= 1500) {
        state = STATE_BUFFERING;
    } else {
        state = STATE_PLAYING;
    }
}

void VideoPlayer::ResetVideo() {
    if (tsPlayed) {
        dsyslog("VideoPlayer::ResetVideo");
        DeviceClear();
        DevicePlay();
        tsPlayed = false;
    }

    bufsize = 0;
    ResetStatus();
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
    if (pause && !mse) {
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
    ScanPts(data, len);

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

// Select a track by its index in the PID-ordered audio/subtitle list.
// This matches the order exported to HbbTV clients.
bool VideoPlayer::SelectTrack(bool subtitle, int index) {
    cDevice *device = cDevice::PrimaryDevice();
    if (device == nullptr)
        return false;

    if (subtitle && index < 0)
        return device->SetCurrentSubtitleTrack(ttNone, true);

    std::vector<std::pair<uint16_t, eTrackType>> tracks;

    if (subtitle) {
        for (int t = ttSubtitleFirst; t <= ttSubtitleLast; ++t) {
            const tTrackId *track = device->GetTrack(eTrackType(t));
            if (track != nullptr && track->id)
                tracks.emplace_back(track->id, eTrackType(t));
        }
    } else {
        for (int t = ttAudioFirst; t <= ttDolbyLast; ++t) {
            const tTrackId *track = device->GetTrack(eTrackType(t));
            if (track != nullptr && track->id)
                tracks.emplace_back(track->id, eTrackType(t));
        }
    }

    std::sort(tracks.begin(), tracks.end());

    if (index < 0 || index >= static_cast<int>(tracks.size())) {
        esyslog("[vdrweb] %s track %d not available (%d tracks)",
                subtitle ? "subtitle" : "audio",
                index,
                static_cast<int>(tracks.size()));
        return false;
    }

    dsyslog("[vdrweb] select %s track %d, pid %d",
            subtitle ? "subtitle" : "audio",
            index,
            tracks[index].first);

    if (subtitle)
        return device->SetCurrentSubtitleTrack(tracks[index].second, true);

    return device->SetCurrentAudioTrack(tracks[index].second);
}
