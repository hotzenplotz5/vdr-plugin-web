#include "vdrsuite_hbbtv_presentation_service.h"

#include "../qoi.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace
{

constexpr std::uint32_t MaximumRenderWidth = 3840U;
constexpr std::uint32_t MaximumRenderHeight = 2160U;
constexpr std::size_t MaximumEncodedBytes = 16U * 1024U * 1024U;

struct PresentationState {
    std::string sessionId;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint64_t frameRevision = 0;
    std::uint64_t observedAt = 0;
    std::uint64_t encodedRevision = 0;
    std::vector<std::uint8_t> bgra;
    std::vector<std::uint8_t> encodedQoi;
};

std::mutex presentationMutex;
PresentationState presentation;

std::uint64_t unixSeconds()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

bool boundedText(
    const char* value,
    std::size_t capacity,
    std::string& output)
{
    if (value == nullptr || capacity == 0) return false;
    std::size_t length = 0;
    while (length < capacity && value[length] != '\0') ++length;
    if (length == 0 || length == capacity) return false;
    output.assign(value, length);
    return true;
}

template <std::size_t N>
void copyText(char (&target)[N], const std::string& source)
{
    std::memset(target, 0, N);
    const std::size_t length = std::min(source.size(), N - 1U);
    std::memcpy(target, source.data(), length);
    target[length] = '\0';
}

void resetResponse(
    VdrWebHbbtvPresentationV1& response,
    const VdrWebHbbtvPresentationV1& request)
{
    std::memset(&response, 0, sizeof(response));
    response.structSize = sizeof(response);
    response.operation = request.operation;
    response.frameRevision = request.frameRevision;
    response.offset = request.offset;
    std::size_t sessionLength = 0;
    while (sessionLength < VDRWEB_HBBTV_SESSION_ID_MAX &&
           request.sessionId[sessionLength] != '\0') {
        ++sessionLength;
    }
    copyText(
        response.sessionId,
        std::string(request.sessionId, sessionLength));
    response.schemaVersion = VDRWEB_HBBTV_PRESENTATION_SCHEMA_V1;
    response.result = VDRWEB_HBBTV_PRESENTATION_RESULT_INVALID_REQUEST;
}

bool validDimensions(
    int renderWidth,
    int renderHeight,
    int x,
    int y,
    int width,
    int height)
{
    if (renderWidth <= 0 || renderHeight <= 0 ||
        width <= 0 || height <= 0 ||
        renderWidth > static_cast<int>(MaximumRenderWidth) ||
        renderHeight > static_cast<int>(MaximumRenderHeight) ||
        x < 0 || y < 0 ||
        x > renderWidth - width ||
        y > renderHeight - height)
    {
        return false;
    }
    return true;
}

bool ensureEncodedSnapshot(PresentationState& state)
{
    if (state.frameRevision == 0 ||
        state.bgra.empty() ||
        state.renderWidth == 0 ||
        state.renderHeight == 0)
    {
        return false;
    }

    if (state.encodedRevision == state.frameRevision &&
        !state.encodedQoi.empty())
    {
        return true;
    }

    const std::size_t pixelCount =
        static_cast<std::size_t>(state.renderWidth) *
        static_cast<std::size_t>(state.renderHeight);
    if (pixelCount >
        std::numeric_limits<std::size_t>::max() / 4U)
    {
        return false;
    }

    std::vector<std::uint8_t> rgba(pixelCount * 4U);
    for (std::size_t index = 0; index < pixelCount; ++index)
    {
        const std::size_t offset = index * 4U;
        rgba[offset + 0U] = state.bgra[offset + 2U];
        rgba[offset + 1U] = state.bgra[offset + 1U];
        rgba[offset + 2U] = state.bgra[offset + 0U];
        rgba[offset + 3U] = state.bgra[offset + 3U];
    }

    qoi_desc description{
        state.renderWidth,
        state.renderHeight,
        4,
        QOI_SRGB
    };

    int encodedLength = 0;
    void* encoded = qoi_encode(
        rgba.data(),
        &description,
        &encodedLength);
    if (encoded == nullptr || encodedLength <= 0)
    {
        if (encoded != nullptr) free(encoded);
        return false;
    }

    const std::size_t size =
        static_cast<std::size_t>(encodedLength);
    if (size > MaximumEncodedBytes)
    {
        free(encoded);
        state.encodedQoi.clear();
        state.encodedRevision = 0;
        return false;
    }

    const auto* bytes =
        static_cast<const std::uint8_t*>(encoded);
    state.encodedQoi.assign(bytes, bytes + size);
    state.encodedRevision = state.frameRevision;
    free(encoded);
    return true;
}

} // namespace

void VdrSuiteHbbtvPresentationStore::BeginSession(
    const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(presentationMutex);
    presentation = {};
    if (!sessionId.empty() &&
        sessionId.size() < VDRWEB_HBBTV_SESSION_ID_MAX)
    {
        presentation.sessionId = sessionId;
    }
}

void VdrSuiteHbbtvPresentationStore::EndSession(
    const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(presentationMutex);
    if (!sessionId.empty() &&
        presentation.sessionId != sessionId)
    {
        return;
    }
    presentation = {};
}

bool VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(
    const std::uint8_t* image,
    int renderWidth,
    int renderHeight,
    int x,
    int y,
    int width,
    int height)
{
    if (image == nullptr ||
        !validDimensions(
            renderWidth,
            renderHeight,
            x,
            y,
            width,
            height))
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(presentationMutex);
    if (presentation.sessionId.empty())
    {
        return true;
    }

    const std::uint32_t rw =
        static_cast<std::uint32_t>(renderWidth);
    const std::uint32_t rh =
        static_cast<std::uint32_t>(renderHeight);

    if (presentation.renderWidth != rw ||
        presentation.renderHeight != rh)
    {
        const std::size_t pixelCount =
            static_cast<std::size_t>(rw) *
            static_cast<std::size_t>(rh);
        if (pixelCount >
            std::numeric_limits<std::size_t>::max() / 4U)
        {
            return false;
        }

        presentation.renderWidth = rw;
        presentation.renderHeight = rh;
        presentation.bgra.assign(pixelCount * 4U, 0U);
        presentation.frameRevision = 0;
        presentation.encodedRevision = 0;
        presentation.encodedQoi.clear();
    }

    const std::size_t sourceStride =
        static_cast<std::size_t>(width) * 4U;
    const std::size_t destinationStride =
        static_cast<std::size_t>(renderWidth) * 4U;

    for (int row = 0; row < height; ++row)
    {
        const std::size_t sourceOffset =
            static_cast<std::size_t>(row) * sourceStride;
        const std::size_t destinationOffset =
            (static_cast<std::size_t>(y + row) *
                 destinationStride) +
            static_cast<std::size_t>(x) * 4U;

        std::memcpy(
            presentation.bgra.data() + destinationOffset,
            image + sourceOffset,
            sourceStride);
    }

    ++presentation.frameRevision;
    if (presentation.frameRevision == 0)
        presentation.frameRevision = 1;
    presentation.observedAt = unixSeconds();
    presentation.encodedRevision = 0;
    presentation.encodedQoi.clear();
    return true;
}

bool VdrSuiteHbbtvPresentationStore::Read(
    VdrWebHbbtvPresentationV1& message)
{
    const VdrWebHbbtvPresentationV1 request = message;
    resetResponse(message, request);

    std::string sessionId;
    if (request.structSize != sizeof(request) ||
        (request.operation != VDRWEB_HBBTV_PRESENTATION_META &&
         request.operation != VDRWEB_HBBTV_PRESENTATION_CHUNK) ||
        !boundedText(
            request.sessionId,
            VDRWEB_HBBTV_SESSION_ID_MAX,
            sessionId))
    {
        message.result =
            VDRWEB_HBBTV_PRESENTATION_RESULT_INVALID_REQUEST;
        return true;
    }

    std::lock_guard<std::mutex> lock(presentationMutex);
    if (presentation.sessionId.empty())
    {
        message.result =
            VDRWEB_HBBTV_PRESENTATION_RESULT_NO_SESSION;
        return true;
    }
    if (presentation.sessionId != sessionId)
    {
        message.result =
            VDRWEB_HBBTV_PRESENTATION_RESULT_SESSION_MISMATCH;
        return true;
    }
    if (presentation.frameRevision == 0 ||
        presentation.bgra.empty())
    {
        message.result =
            VDRWEB_HBBTV_PRESENTATION_RESULT_NO_FRAME;
        return true;
    }

    if (!ensureEncodedSnapshot(presentation))
    {
        message.result =
            VDRWEB_HBBTV_PRESENTATION_RESULT_FRAME_TOO_LARGE;
        return true;
    }

    message.frameRevision = presentation.frameRevision;
    message.observedAt = presentation.observedAt;
    message.renderWidth = presentation.renderWidth;
    message.renderHeight = presentation.renderHeight;
    message.encodedBytes =
        static_cast<std::uint32_t>(
            presentation.encodedQoi.size());

    if (request.operation == VDRWEB_HBBTV_PRESENTATION_META)
    {
        message.result =
            VDRWEB_HBBTV_PRESENTATION_RESULT_OK;
        return true;
    }

    if (request.frameRevision != presentation.frameRevision)
    {
        message.result =
            VDRWEB_HBBTV_PRESENTATION_RESULT_REVISION_MISMATCH;
        return true;
    }

    const std::size_t offset = request.offset;
    if (offset >= presentation.encodedQoi.size())
    {
        message.result =
            VDRWEB_HBBTV_PRESENTATION_RESULT_RANGE_INVALID;
        return true;
    }

    const std::size_t remaining =
        presentation.encodedQoi.size() - offset;
    const std::size_t returned =
        std::min<std::size_t>(
            remaining,
            VDRWEB_HBBTV_PRESENTATION_CHUNK_MAX);

    std::memcpy(
        message.data,
        presentation.encodedQoi.data() + offset,
        returned);
    message.returnedBytes =
        static_cast<std::uint32_t>(returned);
    message.result =
        VDRWEB_HBBTV_PRESENTATION_RESULT_OK;
    return true;
}
