#include "../service/vdrsuite_hbbtv_presentation_service.h"
#include "../qoi.h"

#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::vector<VdrWebHbbtvPresentationChangedV1> notifications;
void VdrSuiteHbbtvPublishPresentationChanged(VdrWebHbbtvPresentationChangedV1& event)
{
    notifications.push_back(event);
}

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

VdrWebHbbtvPresentationV2 request(
    std::uint8_t operation,
    const char* sessionId,
    std::uint64_t revision = 0,
    std::uint32_t offset = 0)
{
    VdrWebHbbtvPresentationV2 message{};
    message.structSize = sizeof(message);
    message.operation = operation;
    message.frameRevision = revision;
    message.offset = offset;
    copyText(message.sessionId, sessionId);
    return message;
}

std::vector<std::uint8_t> readPayload(
    const VdrWebHbbtvPresentationV2& meta)
{
    std::vector<std::uint8_t> encoded;
    std::uint32_t offset = 0;
    while (offset < meta.encodedBytes)
    {
        auto chunk = request(
            VDRWEB_HBBTV_PRESENTATION_CHUNK,
            meta.sessionId,
            meta.frameRevision,
            offset);
        assert(VdrSuiteHbbtvPresentationStore::Read(chunk));
        assert(chunk.result == VDRWEB_HBBTV_PRESENTATION_RESULT_OK);
        assert(chunk.frameRevision == meta.frameRevision);
        assert(chunk.baseRevision == meta.baseRevision);
        assert(chunk.frameKind == meta.frameKind);
        assert(chunk.patchX == meta.patchX);
        assert(chunk.patchY == meta.patchY);
        assert(chunk.patchWidth == meta.patchWidth);
        assert(chunk.patchHeight == meta.patchHeight);
        assert(chunk.returnedBytes > 0);
        encoded.insert(
            encoded.end(),
            chunk.data,
            chunk.data + chunk.returnedBytes);
        offset += chunk.returnedBytes;
    }
    assert(encoded.size() == meta.encodedBytes);
    return encoded;
}

std::vector<std::uint8_t> decode(
    const std::vector<std::uint8_t>& encoded,
    std::uint32_t width,
    std::uint32_t height)
{
    qoi_desc description{};
    void* pixels = qoi_decode(
        encoded.data(),
        static_cast<int>(encoded.size()),
        &description,
        4);
    assert(pixels != nullptr);
    assert(description.width == width);
    assert(description.height == height);

    const std::size_t size =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) * 4U;
    const auto* begin = static_cast<const std::uint8_t*>(pixels);
    std::vector<std::uint8_t> result(begin, begin + size);
    free(pixels);
    return result;
}

} // namespace

int main()
{
    VdrSuiteHbbtvPresentationStore::EndSession();
    VdrSuiteHbbtvPresentationStore::BeginSession("session-overlay");

    const std::uint8_t firstPatch[] = {
        // BGRA red, transparent, green, blue.
        0x00, 0x00, 0xff, 0xff,
        0x9a, 0x2e, 0xfe, 0x00,
        0x00, 0xff, 0x00, 0xff,
        0xff, 0x00, 0x00, 0xff,
    };
    assert(VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
        firstPatch, 2, 2, 0, 0, 2, 2));

    auto full = request(
        VDRWEB_HBBTV_PRESENTATION_META,
        "session-overlay",
        0);
    assert(VdrSuiteHbbtvPresentationStore::Read(full));
    assert(full.schemaVersion == VDRWEB_HBBTV_PRESENTATION_SCHEMA_V2);
    assert(full.result == VDRWEB_HBBTV_PRESENTATION_RESULT_OK);
    assert(full.frameRevision == 1);
    assert(full.baseRevision == 0);
    assert(full.frameKind == VDRWEB_HBBTV_PRESENTATION_FRAME_FULL);
    assert(full.renderWidth == 2);
    assert(full.renderHeight == 2);
    assert(full.patchX == 0);
    assert(full.patchY == 0);
    assert(full.patchWidth == 2);
    assert(full.patchHeight == 2);
    assert(full.encodedBytes > 0);

    const auto fullPixels = decode(readPayload(full), 2, 2);
    assert(fullPixels[0] == 0xff && fullPixels[1] == 0x00 &&
           fullPixels[2] == 0x00 && fullPixels[3] == 0xff);
    assert(fullPixels[4] == 0xfe && fullPixels[5] == 0x2e &&
           fullPixels[6] == 0x9a && fullPixels[7] == 0x00);

    const std::uint8_t yellow[] = {
        0x00, 0xff, 0xff, 0xff
    };
    assert(VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
        yellow, 2, 2, 1, 0, 1, 1));

    auto patch = request(
        VDRWEB_HBBTV_PRESENTATION_META,
        "session-overlay",
        1);
    assert(VdrSuiteHbbtvPresentationStore::Read(patch));
    assert(patch.result == VDRWEB_HBBTV_PRESENTATION_RESULT_OK);
    assert(patch.frameRevision == 2);
    assert(patch.baseRevision == 1);
    assert(patch.frameKind == VDRWEB_HBBTV_PRESENTATION_FRAME_PATCH);
    assert(patch.renderWidth == 2);
    assert(patch.renderHeight == 2);
    assert(patch.patchX == 1);
    assert(patch.patchY == 0);
    assert(patch.patchWidth == 1);
    assert(patch.patchHeight == 1);

    // A newer provider patch must not invalidate the patch snapshot selected
    // by META while the consumer is still reading revision 2.
    const std::uint8_t cyan[] = {
        0xff, 0xff, 0x00, 0xff
    };
    assert(VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
        cyan, 2, 2, 0, 1, 1, 1));

    const auto patchPixels = decode(readPayload(patch), 1, 1);
    assert(patchPixels[0] == 0xff);
    assert(patchPixels[1] == 0xff);
    assert(patchPixels[2] == 0x00);
    assert(patchPixels[3] == 0xff);

    auto nextPatch = request(
        VDRWEB_HBBTV_PRESENTATION_META,
        "session-overlay",
        2);
    assert(VdrSuiteHbbtvPresentationStore::Read(nextPatch));
    assert(nextPatch.result == VDRWEB_HBBTV_PRESENTATION_RESULT_OK);
    assert(nextPatch.frameRevision == 3);
    assert(nextPatch.baseRevision == 2);
    assert(nextPatch.frameKind == VDRWEB_HBBTV_PRESENTATION_FRAME_PATCH);
    assert(nextPatch.patchX == 0);
    assert(nextPatch.patchY == 1);
    assert(nextPatch.patchWidth == 1);
    assert(nextPatch.patchHeight == 1);

    auto unchanged = request(
        VDRWEB_HBBTV_PRESENTATION_META,
        "session-overlay",
        3);
    assert(VdrSuiteHbbtvPresentationStore::Read(unchanged));
    assert(unchanged.result == VDRWEB_HBBTV_PRESENTATION_RESULT_OK);
    assert(unchanged.frameRevision == 3);
    assert(unchanged.frameKind == VDRWEB_HBBTV_PRESENTATION_FRAME_NONE);
    assert(unchanged.baseRevision == 3);
    assert(unchanged.encodedBytes == 0);

    auto staleChunk = request(
        VDRWEB_HBBTV_PRESENTATION_CHUNK,
        "session-overlay",
        2,
        0);
    assert(VdrSuiteHbbtvPresentationStore::Read(staleChunk));
    assert(
        staleChunk.result ==
        VDRWEB_HBBTV_PRESENTATION_RESULT_REVISION_MISMATCH);

    const std::uint8_t clearPatch[] = {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    assert(VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
        clearPatch, 2, 2, 0, 0, 2, 2));

    auto resync = request(
        VDRWEB_HBBTV_PRESENTATION_META,
        "session-overlay",
        0);
    assert(VdrSuiteHbbtvPresentationStore::Read(resync));
    assert(resync.result == VDRWEB_HBBTV_PRESENTATION_RESULT_OK);
    assert(resync.frameRevision == 4);
    assert(resync.baseRevision == 0);
    assert(resync.frameKind == VDRWEB_HBBTV_PRESENTATION_FRAME_FULL);
    assert(
        resync.visibility ==
        VDRWEB_HBBTV_PRESENTATION_VISIBILITY_HIDDEN);

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

    {
    // Browser QOI updates must reach the Suite with no local WebOSDPage.
    VdrSuiteHbbtvPresentationStore::BeginSession("remote-video");
    const unsigned char pixel[] = {0x11, 0x22, 0x33, 0xff};
    qoi_desc desc{};
    desc.width = desc.height = 1;
    desc.channels = 4;
    int encodedSize = 0;
    void* remoteEncoded = qoi_encode(pixel, &desc, &encodedSize);
    assert(remoteEncoded != nullptr);
    std::string patch(static_cast<const char*>(remoteEncoded), encodedSize);
    std::free(remoteEncoded);
    assert(VdrSuiteHbbtvPresentationStore::ApplyQoiPatch(patch, 2, 2, 1, 1));
    auto remoteFrame = request(VDRWEB_HBBTV_PRESENTATION_META, "remote-video");
    assert(VdrSuiteHbbtvPresentationStore::Read(remoteFrame));
    assert(remoteFrame.result == VDRWEB_HBBTV_PRESENTATION_RESULT_OK);
    assert(remoteFrame.visibility == VDRWEB_HBBTV_PRESENTATION_VISIBILITY_VISIBLE);
    assert(!VdrSuiteHbbtvPresentationStore::ApplyQoiPatch("invalid", 2, 2, 0, 0));
    patch[4] = 0x7f;
    assert(!VdrSuiteHbbtvPresentationStore::ApplyQoiPatch(patch, 2, 2, 0, 0));
    VdrSuiteHbbtvPresentationStore::EndSession("remote-video");
    }
    assert(notifications.size() == 7);
    assert(notifications.front().schemaVersion == VDRWEB_HBBTV_PRESENTATION_CHANGED_SCHEMA_V1);
    assert(notifications.front().frameRevision == 1);
    assert(std::strcmp(notifications.front().sessionId, "session-overlay") == 0);
    assert(notifications.back().frameRevision == 1);
    assert(std::strcmp(notifications.back().sessionId, "remote-video") == 0);
    return 0;
}
