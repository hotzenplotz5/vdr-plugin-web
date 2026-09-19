#include "service/vdrsuite_hbbtv_media_service.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
VdrWebHbbtvMediaV1 readMedia(const char* sessionId)
{
    VdrWebHbbtvMediaV1 message {};
    message.structSize = sizeof(message);
    std::strncpy(
        message.sessionId,
        sessionId,
        sizeof(message.sessionId) - 1);
    assert(VdrSuiteHbbtvMediaStore::Read(message));
    return message;
}

int connectSocket(const char* path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(
        address.sun_path,
        path,
        sizeof(address.sun_path) - 1);

    int attempts = 100;
    while (attempts-- > 0) {
        if (::connect(
                fd,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == 0) {
            return fd;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(10));
    }

    assert(false);
    return -1;
}

std::vector<std::uint8_t> receiveBytes(
    int fd,
    std::size_t wanted)
{
    std::vector<std::uint8_t> result(wanted);
    std::size_t offset = 0;
    while (offset < wanted) {
        const ssize_t received = ::recv(
            fd,
            result.data() + offset,
            wanted - offset,
            0);
        assert(received > 0);
        offset += static_cast<std::size_t>(received);
    }
    return result;
}
}

int main()
{
    const std::string root =
        "/tmp/vdrsuite-hbbtv-media-test-" +
        std::to_string(static_cast<long long>(::getpid()));
    assert(::setenv(
        "VDR_SUITE_HBBTV_MEDIA_SOCKET_DIR",
        root.c_str(),
        1) == 0);

    VdrSuiteHbbtvMediaStore::BeginSession("session-media");

    auto idle = readMedia("session-media");
    assert(idle.result == VDRWEB_HBBTV_MEDIA_RESULT_OK);
    assert(idle.state == VDRWEB_HBBTV_MEDIA_STATE_NONE);
    assert(idle.mediaRevision == 0);
    assert(idle.socketPath[0] == '\0');

    assert(VdrSuiteHbbtvMediaStore::BeginVideo("video-info-a"));
    auto streaming = readMedia("session-media");
    assert(streaming.result == VDRWEB_HBBTV_MEDIA_RESULT_OK);
    assert(streaming.state == VDRWEB_HBBTV_MEDIA_STATE_STREAMING);
    assert(streaming.mediaRevision == 1);
    assert(streaming.fullscreen == 1);
    assert(streaming.socketPath[0] != '\0');

    const std::string firstPath(streaming.socketPath);
    const int firstClient = connectSocket(streaming.socketPath);

    std::vector<std::uint8_t> firstPayload(188U * 2U, 0);
    firstPayload[0] = 0x47;
    firstPayload[188] = 0x47;
    assert(VdrSuiteHbbtvMediaStore::AppendTs(
        firstPayload.data(),
        firstPayload.size()));
    assert(receiveBytes(
        firstClient,
        firstPayload.size()) == firstPayload);

    int connectedAttempts = 100;
    while (connectedAttempts-- > 0) {
        streaming = readMedia("session-media");
        if (streaming.consumerConnected == 1)
            break;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(10));
    }
    assert(streaming.consumerConnected == 1);

    VdrSuiteHbbtvMediaStore::PauseVideo();
    auto paused = readMedia("session-media");
    assert(paused.state == VDRWEB_HBBTV_MEDIA_STATE_PAUSED);

    VdrSuiteHbbtvMediaStore::SetVideoSize(
        100, 50, 640, 360);
    auto windowed = readMedia("session-media");
    assert(windowed.fullscreen == 0);
    assert(windowed.x == 100);
    assert(windowed.y == 50);
    assert(windowed.width == 640);
    assert(windowed.height == 360);

    VdrSuiteHbbtvMediaStore::ResumeVideo();
    assert(
        readMedia("session-media").state ==
        VDRWEB_HBBTV_MEDIA_STATE_STREAMING);

    assert(VdrSuiteHbbtvMediaStore::ResetVideo("video-info-b"));
    auto reset = readMedia("session-media");
    assert(reset.mediaRevision == 2);
    assert(reset.state == VDRWEB_HBBTV_MEDIA_STATE_STREAMING);
    assert(std::string(reset.socketPath) != firstPath);
    ::close(firstClient);

    const int secondClient = connectSocket(reset.socketPath);
    std::vector<std::uint8_t> secondPayload(188U, 0);
    secondPayload[0] = 0x47;
    assert(VdrSuiteHbbtvMediaStore::AppendTs(
        secondPayload.data(),
        secondPayload.size()));
    assert(receiveBytes(
        secondClient,
        secondPayload.size()) == secondPayload);

    VdrSuiteHbbtvMediaStore::SetVideoFullscreen();
    auto fullscreen = readMedia("session-media");
    assert(fullscreen.fullscreen == 1);
    assert(fullscreen.x == 0);
    assert(fullscreen.y == 0);
    assert(fullscreen.width == 0);
    assert(fullscreen.height == 0);

    VdrSuiteHbbtvMediaStore::StopVideo();
    auto stopped = readMedia("session-media");
    assert(stopped.state == VDRWEB_HBBTV_MEDIA_STATE_STOPPED);
    assert(stopped.socketPath[0] == '\0');
    ::close(secondClient);

    auto wrong = readMedia("other-session");
    assert(
        wrong.result ==
        VDRWEB_HBBTV_MEDIA_RESULT_SESSION_MISMATCH);

    VdrSuiteHbbtvMediaStore::EndSession("session-media");
    auto ended = readMedia("session-media");
    assert(ended.result == VDRWEB_HBBTV_MEDIA_RESULT_NO_SESSION);

    ::rmdir(root.c_str());

    return 0;
}
