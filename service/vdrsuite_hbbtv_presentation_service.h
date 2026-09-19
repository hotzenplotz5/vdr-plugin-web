#pragma once

#include "vdrsuite_hbbtv_runtime_service.h"

#include <cstddef>
#include <cstdint>
#include <string>

#define VDRWEB_SERVICE_HBBTV_PRESENTATION_V1 "VdrWeb::HbbtvPresentation-v1"
#define VDRWEB_HBBTV_PRESENTATION_SCHEMA_V1 1U
#define VDRWEB_HBBTV_PRESENTATION_CHUNK_MAX 49152U

enum VdrWebHbbtvPresentationOperationV1 : std::uint8_t {
    VDRWEB_HBBTV_PRESENTATION_META = 1,
    VDRWEB_HBBTV_PRESENTATION_CHUNK = 2
};

enum VdrWebHbbtvPresentationResultV1 : std::uint8_t {
    VDRWEB_HBBTV_PRESENTATION_RESULT_OK = 0,
    VDRWEB_HBBTV_PRESENTATION_RESULT_INVALID_REQUEST = 1,
    VDRWEB_HBBTV_PRESENTATION_RESULT_NO_SESSION = 2,
    VDRWEB_HBBTV_PRESENTATION_RESULT_SESSION_MISMATCH = 3,
    VDRWEB_HBBTV_PRESENTATION_RESULT_NO_FRAME = 4,
    VDRWEB_HBBTV_PRESENTATION_RESULT_REVISION_MISMATCH = 5,
    VDRWEB_HBBTV_PRESENTATION_RESULT_RANGE_INVALID = 6,
    VDRWEB_HBBTV_PRESENTATION_RESULT_FRAME_TOO_LARGE = 7
};

enum VdrWebHbbtvPresentationVisibilityV1 : std::uint8_t {
    VDRWEB_HBBTV_PRESENTATION_VISIBILITY_UNKNOWN = 0,
    VDRWEB_HBBTV_PRESENTATION_VISIBILITY_HIDDEN = 1,
    VDRWEB_HBBTV_PRESENTATION_VISIBILITY_VISIBLE = 2
};

struct VdrWebHbbtvPresentationV1 {
    std::uint32_t structSize;

    // Request.
    std::uint8_t operation;
    std::uint8_t reservedRequest[3];
    std::uint64_t frameRevision;
    std::uint32_t offset;
    char sessionId[VDRWEB_HBBTV_SESSION_ID_MAX];

    // Response.
    std::uint32_t schemaVersion;
    std::uint8_t result;
    std::uint8_t visibility;
    std::uint8_t reservedResponse[2];
    std::uint64_t observedAt;
    std::uint32_t renderWidth;
    std::uint32_t renderHeight;
    std::uint32_t encodedBytes;
    std::uint32_t returnedBytes;
    std::uint8_t data[VDRWEB_HBBTV_PRESENTATION_CHUNK_MAX];
};

class VdrSuiteHbbtvPresentationStore final {
public:
    static void BeginSession(const std::string& sessionId);
    static void EndSession(const std::string& sessionId = {});

    static bool BeginClose(
        const std::string& sessionId,
        std::uint64_t nowMilliseconds);
    static void CancelClose(const std::string& sessionId);
    static VdrSuiteHbbtvRuntimeCloseConfirmation CloseConfirmation(
        const std::string& sessionId,
        std::uint64_t nowMilliseconds);

    static bool ApplyBgraPatch(
        const std::uint8_t* image,
        int renderWidth,
        int renderHeight,
        int x,
        int y,
        int width,
        int height);

    static bool Read(VdrWebHbbtvPresentationV1& message);
};
