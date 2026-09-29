#pragma once

#include "vdrsuite_hbbtv_runtime_service.h"

#include <cstddef>
#include <cstdint>
#include <string>

#define VDRWEB_SERVICE_HBBTV_MEDIA_V1 "VdrWeb::HbbtvMedia-v1"
#define VDRWEB_HBBTV_MEDIA_SCHEMA_V1 1U
#define VDRWEB_HBBTV_MEDIA_SOCKET_PATH_MAX 108U

enum VdrWebHbbtvMediaResultV1 : std::uint8_t {
    VDRWEB_HBBTV_MEDIA_RESULT_OK = 0,
    VDRWEB_HBBTV_MEDIA_RESULT_INVALID_REQUEST = 1,
    VDRWEB_HBBTV_MEDIA_RESULT_NO_SESSION = 2,
    VDRWEB_HBBTV_MEDIA_RESULT_SESSION_MISMATCH = 3
};

enum VdrWebHbbtvMediaStateV1 : std::uint8_t {
    VDRWEB_HBBTV_MEDIA_STATE_NONE = 0,
    VDRWEB_HBBTV_MEDIA_STATE_STREAMING = 1,
    VDRWEB_HBBTV_MEDIA_STATE_PAUSED = 2,
    VDRWEB_HBBTV_MEDIA_STATE_STOPPED = 3,
    VDRWEB_HBBTV_MEDIA_STATE_FAILED = 4
};

struct VdrWebHbbtvMediaV1 {
    std::uint32_t structSize;

    // Request.
    char sessionId[VDRWEB_HBBTV_SESSION_ID_MAX];

    // Response. socketPath is private provider topology and must never be
    // copied to a public browser-facing contract.
    std::uint32_t schemaVersion;
    std::uint8_t result;
    std::uint8_t state;
    std::uint8_t fullscreen;
    std::uint8_t consumerConnected;
    std::uint64_t mediaRevision;
    std::int32_t x;
    std::int32_t y;
    std::int32_t width;
    std::int32_t height;
    char socketPath[VDRWEB_HBBTV_MEDIA_SOCKET_PATH_MAX];
};

class VdrSuiteHbbtvMediaStore final {
public:
    static void BeginSession(const std::string& sessionId);
    static void EndSession(const std::string& sessionId = {});

    static bool BeginVideo(const std::string& videoInfo);
    static bool ResetVideo(const std::string& videoInfo);
    static void StopVideo();
    static void PauseVideo();
    static void ResumeVideo();

    static void SetVideoSize(int x, int y, int width, int height);
    static void SetVideoFullscreen();

    static bool AppendTs(const std::uint8_t* data, std::size_t size);
    static bool Read(VdrWebHbbtvMediaV1& message);
};
