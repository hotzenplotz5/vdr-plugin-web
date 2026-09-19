#include "../service/vdrsuite_hbbtv_presentation_service.h"
#include "../qoi.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{

template <std::size_t N>
void copyText(char (&target)[N], const char* value)
{
    const std::size_t length = std::strlen(value);
    assert(length < N);
    std::memset(target, 0, N);
    std::memcpy(target, value, length + 1U);
}

VdrWebHbbtvPresentationV1 request(
    std::uint8_t operation,
    const char* sessionId,
    std::uint64_t revision = 0,
    std::uint32_t offset = 0)
{
    VdrWebHbbtvPresentationV1 message{};
    message.structSize = sizeof(message);
    message.operation = operation;
    message.frameRevision = revision;
    message.offset = offset;
    copyText(message.sessionId, sessionId);
    return message;
}

}

int main()
{
    VdrSuiteHbbtvPresentationStore::EndSession();
    VdrSuiteHbbtvPresentationStore::BeginSession("session-overlay");

    const std::uint8_t firstPatch[] = {
        // BGRA red, transparent.
        0x00, 0x00, 0xff, 0xff,
        0x9a, 0x2e, 0xfe, 0x00,
        // BGRA green, blue.
        0x00, 0xff, 0x00, 0xff,
        0xff, 0x00, 0x00, 0xff,
    };
    assert(VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
        firstPatch, 2, 2, 0, 0, 2, 2));

    auto meta = request(
        VDRWEB_HBBTV_PRESENTATION_META,
        "session-overlay");
    assert(VdrSuiteHbbtvPresentationStore::Read(meta));
    assert(meta.schemaVersion == VDRWEB_HBBTV_PRESENTATION_SCHEMA_V1);
    assert(meta.result == VDRWEB_HBBTV_PRESENTATION_RESULT_OK);
    assert(
        meta.visibility ==
        VDRWEB_HBBTV_PRESENTATION_VISIBILITY_VISIBLE);
    assert(meta.frameRevision == 1);
    assert(meta.renderWidth == 2);
    assert(meta.renderHeight == 2);
    assert(meta.encodedBytes > 0);

    std::vector<std::uint8_t> encoded;
    std::uint32_t offset = 0;
    while (offset < meta.encodedBytes)
    {
        auto chunk = request(
            VDRWEB_HBBTV_PRESENTATION_CHUNK,
            "session-overlay",
            meta.frameRevision,
            offset);
        assert(VdrSuiteHbbtvPresentationStore::Read(chunk));
        assert(chunk.result == VDRWEB_HBBTV_PRESENTATION_RESULT_OK);
        assert(chunk.returnedBytes > 0);
        encoded.insert(
            encoded.end(),
            chunk.data,
            chunk.data + chunk.returnedBytes);
        offset += chunk.returnedBytes;
    }
    assert(encoded.size() == meta.encodedBytes);

    qoi_desc description{};
    void* decoded = qoi_decode(
        encoded.data(),
        static_cast<int>(encoded.size()),
        &description,
        4);
    assert(decoded != nullptr);
    assert(description.width == 2);
    assert(description.height == 2);

    const auto* rgba = static_cast<const std::uint8_t*>(decoded);
    assert(rgba[0] == 0xff && rgba[1] == 0x00 &&
           rgba[2] == 0x00 && rgba[3] == 0xff);
    assert(rgba[4] == 0xfe && rgba[5] == 0x2e &&
           rgba[6] == 0x9a && rgba[7] == 0x00);
    assert(rgba[8] == 0x00 && rgba[9] == 0xff &&
           rgba[10] == 0x00 && rgba[11] == 0xff);
    assert(rgba[12] == 0x00 && rgba[13] == 0x00 &&
           rgba[14] == 0xff && rgba[15] == 0xff);
    free(decoded);

    const std::uint8_t yellow[] = {
        0x00, 0xff, 0xff, 0xff
    };
    assert(VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
        yellow, 2, 2, 1, 0, 1, 1));

    auto changed = request(
        VDRWEB_HBBTV_PRESENTATION_META,
        "session-overlay");
    assert(VdrSuiteHbbtvPresentationStore::Read(changed));
    assert(changed.result == VDRWEB_HBBTV_PRESENTATION_RESULT_OK);
    assert(
        changed.visibility ==
        VDRWEB_HBBTV_PRESENTATION_VISIBILITY_VISIBLE);
    assert(changed.frameRevision == 2);

    const std::uint8_t clearPatch[] = {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    assert(VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
        clearPatch, 2, 2, 0, 0, 2, 2));

    auto hidden = request(
        VDRWEB_HBBTV_PRESENTATION_META,
        "session-overlay");
    assert(VdrSuiteHbbtvPresentationStore::Read(hidden));
    assert(hidden.result == VDRWEB_HBBTV_PRESENTATION_RESULT_OK);
    assert(
        hidden.visibility ==
        VDRWEB_HBBTV_PRESENTATION_VISIBILITY_HIDDEN);
    assert(hidden.frameRevision == 3);

    assert(VdrSuiteHbbtvPresentationStore::BeginClose(
        "session-overlay", 1000));
    assert(
        VdrSuiteHbbtvPresentationStore::CloseConfirmation(
            "session-overlay", 1001) ==
        VdrSuiteHbbtvRuntimeCloseConfirmation::Pending);

    const std::uint8_t visibleAgain[] = {
        0x00, 0x00, 0xff, 0xff
    };
    assert(VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
        visibleAgain, 2, 2, 0, 0, 1, 1));
    assert(
        VdrSuiteHbbtvPresentationStore::CloseConfirmation(
            "session-overlay", 1002) ==
        VdrSuiteHbbtvRuntimeCloseConfirmation::Pending);

    const std::uint8_t clearAgain[] = {
        0x00, 0x00, 0x00, 0x00
    };
    assert(VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
        clearAgain, 2, 2, 0, 0, 1, 1));
    assert(
        VdrSuiteHbbtvPresentationStore::CloseConfirmation(
            "session-overlay", 1003) ==
        VdrSuiteHbbtvRuntimeCloseConfirmation::Confirmed);

    assert(VdrSuiteHbbtvPresentationStore::BeginClose(
        "session-overlay", 2000));
    assert(
        VdrSuiteHbbtvPresentationStore::CloseConfirmation(
            "session-overlay", 7000) ==
        VdrSuiteHbbtvRuntimeCloseConfirmation::Failed);

    auto staleChunk = request(
        VDRWEB_HBBTV_PRESENTATION_CHUNK,
        "session-overlay",
        2,
        0);
    assert(VdrSuiteHbbtvPresentationStore::Read(staleChunk));
    assert(
        staleChunk.result ==
        VDRWEB_HBBTV_PRESENTATION_RESULT_REVISION_MISMATCH);

    auto wrongSession = request(
        VDRWEB_HBBTV_PRESENTATION_META,
        "other-session");
    assert(VdrSuiteHbbtvPresentationStore::Read(wrongSession));
    assert(
        wrongSession.result ==
        VDRWEB_HBBTV_PRESENTATION_RESULT_SESSION_MISMATCH);

    VdrSuiteHbbtvPresentationStore::EndSession("session-overlay");

    auto ended = request(
        VDRWEB_HBBTV_PRESENTATION_META,
        "session-overlay");
    assert(VdrSuiteHbbtvPresentationStore::Read(ended));
    assert(
        ended.result ==
        VDRWEB_HBBTV_PRESENTATION_RESULT_NO_SESSION);

    return 0;
}
