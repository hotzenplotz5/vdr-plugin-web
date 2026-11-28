#!/usr/bin/env python3
"""Run production StartVideo/IsWebActive with real Suite media ownership.

Only the VDR device/OSD boundary is doubled: a headless native attach drops
the player control, as on the affected host. No live VDR or CEF is contacted.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
web = (root / 'web.cpp').read_text()

def function(signature):
    start = web.index(signature)
    opening = web.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (web[end] == '{') - (web[end] == '}')
        end += 1
    return web[start:end]

source = r'''
#include "service/vdrsuite_hbbtv_media_service.h"
#include <cassert>
#include <cstring>
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
enum { OPEN, CLOSE, PLAYER, REOPEN };
int nextOsdCommand = OPEN, closeCalls = 0, launches = 0, saves = 0;
bool saveTS = false, useOutputDeviceScale = false, headless = true;
const char* currentTSDir = "unused";
void dsyslog(const char*, ...) {}
void esyslog(const char*, ...) {}
void createTSFileName() { ++saves; }
bool MakeDirs(const char*, bool) { return true; }
std::recursive_mutex playerMutex;
std::unique_ptr<VdrSuiteHbbtvRuntimeService> vdrSuiteHbbtvRuntimeService;
std::string videoInfo;
std::atomic<bool> videoWindowClosed{false};
int lastVideoX = 0, lastVideoY = 0, lastVideoWidth = 0, lastVideoHeight = 0;
struct VideoPlayer {
    void SetMse(bool) {}
    static void SetVideoSize(int, int, int, int) {}
};
VideoPlayer* videoPlayer = nullptr;
void releasePlayer() { delete videoPlayer; videoPlayer = nullptr; }
struct WebOSDPage {
    static WebOSDPage* current;
    static WebOSDPage* Get() { return current; }
    static WebOSDPage* Create(bool, int) {
        static WebOSDPage native;
        return current = &native;
    }
    void Display() {}
    void SetPlayer(VideoPlayer*) {}
};
WebOSDPage* WebOSDPage::current = nullptr;
struct cRemote { static void CallPlugin(const char*) { ++closeCalls; } };
struct cControl {
    static void Launch(WebOSDPage*) { ++launches; }
    static void Attach() {
        // VDR's failed headless Attach invokes Shutdown. Its regular worker
        // then releases the native player because the WebOSDPage is gone.
        if (headless) {
            WebOSDPage::current = nullptr;
            delete videoPlayer;
            videoPlayer = nullptr;
        }
    }
};
struct StartVideoType { std::string videoInfo; bool mse = false; };
struct VideoSizeType { int x, y, w, h; };
struct VdrPluginWebServer {
    bool StartVideo(const StartVideoType&);
    bool IsWebActive();
    bool VideoSize(const VideoSizeType&);
};
'''
source += function('bool VdrPluginWebServer::StartVideo(') + '\n'
source += function('bool VdrPluginWebServer::IsWebActive()') + '\n'
source += function('bool VdrPluginWebServer::VideoSize(') + '\n'
source += r'''
int main() {
    VdrPluginWebServer server;
    WebOSDPage application;
    WebOSDPage::current = &application;
    assert(!VdrSuiteHbbtvMediaStore::BeginVideo("no session"));
    const char* channel = "C-1-1051-10301";
    VdrSuiteHbbtvDiscoveryStore::BeginChannel(channel);
    VdrSuiteHbbtvDiscoveryStore::Upsert(channel, 1, 1, 2,
        "HBBTV-Start", "https://example.invalid/", "start.html", "");
    VdrWebHbbtvDiscoveryV1 discovery{};
    discovery.structSize = sizeof(discovery);
    std::strcpy(discovery.channelId, channel);
    assert(VdrSuiteHbbtvDiscoveryStore::Read(discovery));
    VdrSuiteHbbtvRuntimeHooks hooks;
    hooks.scheduleLaunch = [](const std::string&, const std::string&) { return true; };
    vdrSuiteHbbtvRuntimeService = std::make_unique<VdrSuiteHbbtvRuntimeService>(hooks);
    VdrWebHbbtvRuntimeV1 launch{};
    launch.structSize = sizeof(launch);
    launch.operation = VDRWEB_HBBTV_RUNTIME_LAUNCH;
    launch.applicationId = 1;
    launch.descriptorRevision = discovery.discoveryRevision;
    std::strcpy(launch.sessionId, "input-regression");
    std::strcpy(launch.channelId, channel);
    assert(vdrSuiteHbbtvRuntimeService->Handle(launch));
    assert(vdrSuiteHbbtvRuntimeService->CompleteLaunch("input-regression", true));
    VdrSuiteHbbtvMediaStore::BeginSession("input-regression");
    // The local OSD has closed, but Suite still owns this application.
    videoWindowClosed = true;
    assert(server.VideoSize({10, 20, 320, 180}));
    assert(lastVideoWidth == 320);
    for (bool capture : {false, true}) {
        saveTS = capture;
        assert(server.StartVideo({"h264"}));
        VdrWebHbbtvMediaV1 media{};
        media.structSize = sizeof(media);
        std::strcpy(media.sessionId, "input-regression");
        assert(VdrSuiteHbbtvMediaStore::Read(media));
        assert(media.state == VDRWEB_HBBTV_MEDIA_STATE_STREAMING);
        assert(media.socketPath[0] != '\0');
        assert(media.x == 10 && media.y == 20 && media.width == 320 && media.height == 180);
        // CEF must continue to see its application as active after StartVideo.
        assert(WebOSDPage::Get() == &application);
        assert(server.IsWebActive());
        assert(nextOsdCommand == OPEN);
        assert(closeCalls == 0 && launches == 0 && videoPlayer == nullptr);
        VdrSuiteHbbtvMediaStore::StopVideo();
        assert(server.IsWebActive());
    }
    assert(saves == 1);
    VdrSuiteHbbtvMediaStore::EndSession("input-regression");
    vdrSuiteHbbtvRuntimeService.reset();
    assert(server.VideoSize({1, 2, 3, 4}));
    assert(lastVideoWidth == 320); // native stale window ignored
    videoWindowClosed = false;
    // Ordinary native playback must still be reachable without Suite ownership.
    saveTS = false;
    headless = false;
    assert(server.StartVideo({"h264"}));
    assert(closeCalls == 1 && launches == 1 && videoPlayer != nullptr);
    assert(server.IsWebActive());
    delete videoPlayer;
    videoPlayer = nullptr;
    WebOSDPage::current = &application;
    headless = true;
    assert(server.StartVideo({"h264"}));
    assert(closeCalls == 2 && launches == 2);
    assert(!server.IsWebActive());
    nextOsdCommand = REOPEN;
    assert(server.IsWebActive());
}
'''

with tempfile.TemporaryDirectory(prefix='hbbtv-input-') as directory:
    tmp = Path(directory)
    cpp = tmp / 'lifecycle.cpp'
    cpp.write_text(source)
    binary = tmp / 'lifecycle'
    subprocess.run([
        os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra',
        '-pedantic', '-pthread', '-I', str(root), str(cpp),
        str(root / 'service/vdrsuite_hbbtv_media_service.cpp'),
        '-o', str(binary),
    ], check=True)
    env = dict(os.environ, VDR_SUITE_HBBTV_MEDIA_SOCKET_DIR=str(tmp / 'media'))
    subprocess.run([str(binary)], env=env, check=True)
print('RESULT=HBBTV_VIDEO_INPUT_LIFECYCLE_PASS')
