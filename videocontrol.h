#pragma once

#include <cstdint>

#include <vdr/osd.h>
#include <vdr/player.h>
#include <vdr/tools.h>

class VideoPlayer : public cPlayer {
public:
    explicit VideoPlayer();
    ~VideoPlayer() override;

    void Pause();
    void Resume();

    void SetMse(bool on) { mse = on; }

    void PlayPacket(uint8_t *buffer, int len);
    static void SetVideoSize(int x, int y, int width, int height);
    static void setVideoFullscreen();

    void ResetVideo();
    static bool SelectTrack(bool subtitle, int index);

    bool hasTsError() { return tsError; };

    enum {
        STATE_PLAYING = 0,
        STATE_PAUSED = 1,
        STATE_BUFFERING = 2,
        STATE_ENDED = 3
    };

    void SetEndOfStream();
    void GetStatus(std::int64_t &position, int &state);

protected:
    void Activate(bool On) override;

private:
    static void calcVideoPosition(int x, int y, int w, int h, int *newx, int *newy, int *newwidth, int *newheight);
    bool PlayData(uint8_t *data, int len);
    void ScanPts(const uint8_t *data, int len);
    void ResetStatus();

private:
    bool pause;
    bool mse;
    bool tsError;
    bool tsPlayed;

    std::int64_t firstPts;
    std::int64_t lastPts;
    bool endOfStream;
    std::int64_t lastStc;
    cTimeMs stcStall;
    cTimeMs lastData;
};

extern VideoPlayer *videoPlayer;