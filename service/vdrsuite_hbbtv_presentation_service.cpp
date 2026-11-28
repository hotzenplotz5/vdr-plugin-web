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

bool VdrSuiteHbbtvPresentationStore::ApplyQoiPatch(
    const std::string& encoded, int renderWidth, int renderHeight, int x, int y)
{
    if (encoded.size() < 22U || encoded.size() > 16U * 1024U * 1024U)
        return false;
    const auto dimension = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (std::size_t i = offset; i < offset + 4; ++i)
            value = (value << 8U) | static_cast<unsigned char>(encoded[i]);
        return value;
    };
    if (dimension(4) == 0 || dimension(4) > 3840U ||
        dimension(8) == 0 || dimension(8) > 2160U)
        return false;
    qoi_desc desc{};
    void* pixels = qoi_decode(encoded.data(), static_cast<int>(encoded.size()), &desc, 4);
    if (pixels == nullptr)
        return false;
    const bool applied = ApplyBgraPatch(
        static_cast<const std::uint8_t*>(pixels), renderWidth, renderHeight,
        x, y, static_cast<int>(desc.width), static_cast<int>(desc.height));
    std::free(pixels);
    return applied;
}

namespace
{

constexpr std::uint32_t MaximumRenderWidth = 3840U;
constexpr std::uint32_t MaximumRenderHeight = 2160U;
constexpr std::size_t MaximumEncodedBytes = 16U * 1024U * 1024U;
constexpr std::uint64_t CloseConfirmationTimeoutMilliseconds = 5000U;

struct PresentationState {
    std::string sessionId;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint64_t frameRevision = 0;
    std::uint64_t observedAt = 0;
    std::size_t visibleAlphaPixels = 0;
    bool closePending = false;
    std::uint64_t closeAfterRevision = 0;
    std::uint64_t closeDeadlineMilliseconds = 0;
    std::vector<std::uint8_t> bgra;

    // Stable encoded snapshot selected by the latest META request.
    std::uint64_t encodedRevision = 0;
    std::uint64_t encodedBaseRevision = 0;
    std::uint64_t encodedObservedAt = 0;
    std::size_t encodedVisibleAlphaPixels = 0;
    std::uint8_t encodedFrameKind = VDRWEB_HBBTV_PRESENTATION_FRAME_NONE;
    std::uint32_t encodedRenderWidth = 0;
    std::uint32_t encodedRenderHeight = 0;
    std::uint32_t encodedX = 0;
    std::uint32_t encodedY = 0;
    std::uint32_t encodedWidth = 0;
    std::uint32_t encodedHeight = 0;
    std::vector<std::uint8_t> encodedQoi;

    // Bounding rectangle of all changes since the last selected snapshot.
    bool dirty = false;
    std::uint64_t dirtyBaseRevision = 0;
    std::uint32_t dirtyX = 0;
    std::uint32_t dirtyY = 0;
    std::uint32_t dirtyRight = 0;
    std::uint32_t dirtyBottom = 0;
};

std::mutex presentationMutex;
PresentationState presentation;

void publishPresentationChanged(
    const std::string& sessionId,
    std::uint64_t frameRevision)
{
    if (sessionId.empty() ||
        sessionId.size() >= VDRWEB_HBBTV_SESSION_ID_MAX ||
        frameRevision == 0)
    {
        return;
    }

    VdrWebHbbtvPresentationChangedV1 event{};
    event.structSize = sizeof(event);
    event.schemaVersion = VDRWEB_HBBTV_PRESENTATION_CHANGED_SCHEMA_V1;
    event.frameRevision = frameRevision;
    std::memcpy(event.sessionId, sessionId.data(), sessionId.size());
    event.sessionId[sessionId.size()] = '\0';

    // Best-effort in-process hint only. The frame remains available through
    // the existing bounded META/CHUNK readback contract.
    VdrSuiteHbbtvPublishPresentationChanged(event);
}

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
    VdrWebHbbtvPresentationV2& response,
    const VdrWebHbbtvPresentationV2& request)
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
    response.schemaVersion = VDRWEB_HBBTV_PRESENTATION_SCHEMA_V2;
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

void includeDirtyRect(
    PresentationState& state,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height)
{
    const std::uint32_t right = x + width;
    const std::uint32_t bottom = y + height;
    if (!state.dirty)
    {
        state.dirty = true;
        state.dirtyX = x;
        state.dirtyY = y;
        state.dirtyRight = right;
        state.dirtyBottom = bottom;
        return;
    }

    state.dirtyX = std::min(state.dirtyX, x);
    state.dirtyY = std::min(state.dirtyY, y);
    state.dirtyRight = std::max(state.dirtyRight, right);
    state.dirtyBottom = std::max(state.dirtyBottom, bottom);
}

bool encodeSnapshot(
    PresentationState& state,
    std::uint64_t knownRevision)
{
    if (state.frameRevision == 0 ||
        state.bgra.empty() ||
        state.renderWidth == 0 ||
        state.renderHeight == 0)
    {
        return false;
    }

    const bool canPatch =
        knownRevision != 0 &&
        state.dirty &&
        state.dirtyBaseRevision == knownRevision;

    const std::uint32_t x = canPatch ? state.dirtyX : 0U;
    const std::uint32_t y = canPatch ? state.dirtyY : 0U;
    const std::uint32_t width =
        canPatch ? state.dirtyRight - state.dirtyX : state.renderWidth;
    const std::uint32_t height =
        canPatch ? state.dirtyBottom - state.dirtyY : state.renderHeight;

    if (width == 0 || height == 0 ||
        x > state.renderWidth - width ||
        y > state.renderHeight - height)
    {
        return false;
    }

    const std::size_t pixelCount =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height);
    if (pixelCount >
        std::numeric_limits<std::size_t>::max() / 4U)
    {
        return false;
    }

    std::vector<std::uint8_t> rgba(pixelCount * 4U);
    const std::size_t sourceStride =
        static_cast<std::size_t>(state.renderWidth) * 4U;

    for (std::uint32_t row = 0; row < height; ++row)
    {
        for (std::uint32_t column = 0; column < width; ++column)
        {
            const std::size_t sourceOffset =
                (static_cast<std::size_t>(y + row) * sourceStride) +
                static_cast<std::size_t>(x + column) * 4U;
            const std::size_t destinationOffset =
                (static_cast<std::size_t>(row) * width + column) * 4U;

            rgba[destinationOffset + 0U] = state.bgra[sourceOffset + 2U];
            rgba[destinationOffset + 1U] = state.bgra[sourceOffset + 1U];
            rgba[destinationOffset + 2U] = state.bgra[sourceOffset + 0U];
            rgba[destinationOffset + 3U] = state.bgra[sourceOffset + 3U];
        }
    }

    qoi_desc description{
        width,
        height,
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
        return false;
    }

    const auto* bytes =
        static_cast<const std::uint8_t*>(encoded);
    state.encodedQoi.assign(bytes, bytes + size);
    free(encoded);

    state.encodedRevision = state.frameRevision;
    state.encodedBaseRevision = canPatch ? knownRevision : 0;
    state.encodedObservedAt = state.observedAt;
    state.encodedVisibleAlphaPixels = state.visibleAlphaPixels;
    state.encodedFrameKind =
        canPatch
            ? VDRWEB_HBBTV_PRESENTATION_FRAME_PATCH
            : VDRWEB_HBBTV_PRESENTATION_FRAME_FULL;
    state.encodedRenderWidth = state.renderWidth;
    state.encodedRenderHeight = state.renderHeight;
    state.encodedX = x;
    state.encodedY = y;
    state.encodedWidth = width;
    state.encodedHeight = height;

    state.dirty = false;
    state.dirtyBaseRevision = state.encodedRevision;
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

bool VdrSuiteHbbtvPresentationStore::BeginClose(
    const std::string& sessionId,
    std::uint64_t nowMilliseconds)
{
    std::lock_guard<std::mutex> lock(presentationMutex);
    if (sessionId.empty() ||
        presentation.sessionId != sessionId)
    {
        return false;
    }

    presentation.closePending = true;
    presentation.closeAfterRevision = presentation.frameRevision;
    presentation.closeDeadlineMilliseconds =
        nowMilliseconds >
                std::numeric_limits<std::uint64_t>::max() -
                    CloseConfirmationTimeoutMilliseconds
            ? std::numeric_limits<std::uint64_t>::max()
            : nowMilliseconds + CloseConfirmationTimeoutMilliseconds;
    return true;
}

void VdrSuiteHbbtvPresentationStore::CancelClose(
    const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(presentationMutex);
    if (!sessionId.empty() &&
        presentation.sessionId == sessionId)
    {
        presentation.closePending = false;
        presentation.closeAfterRevision = 0;
        presentation.closeDeadlineMilliseconds = 0;
    }
}

VdrSuiteHbbtvRuntimeCloseConfirmation
VdrSuiteHbbtvPresentationStore::CloseConfirmation(
    const std::string& sessionId,
    std::uint64_t nowMilliseconds)
{
    std::lock_guard<std::mutex> lock(presentationMutex);
    if (sessionId.empty() ||
        presentation.sessionId != sessionId ||
        !presentation.closePending)
    {
        return VdrSuiteHbbtvRuntimeCloseConfirmation::Failed;
    }

    if (presentation.frameRevision > presentation.closeAfterRevision &&
        presentation.visibleAlphaPixels == 0U)
    {
        presentation.closePending = false;
        presentation.closeAfterRevision = 0;
        presentation.closeDeadlineMilliseconds = 0;
        return VdrSuiteHbbtvRuntimeCloseConfirmation::Confirmed;
    }

    if (nowMilliseconds >= presentation.closeDeadlineMilliseconds)
    {
        presentation.closePending = false;
        presentation.closeAfterRevision = 0;
        presentation.closeDeadlineMilliseconds = 0;
        return VdrSuiteHbbtvRuntimeCloseConfirmation::Failed;
    }

    return VdrSuiteHbbtvRuntimeCloseConfirmation::Pending;
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

    std::string changedSessionId;
    std::uint64_t changedFrameRevision = 0;
    {
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
        presentation.visibleAlphaPixels = 0;
        presentation.encodedRevision = 0;
        presentation.encodedBaseRevision = 0;
        presentation.encodedObservedAt = 0;
        presentation.encodedVisibleAlphaPixels = 0;
        presentation.encodedFrameKind =
            VDRWEB_HBBTV_PRESENTATION_FRAME_NONE;
        presentation.encodedRenderWidth = 0;
        presentation.encodedRenderHeight = 0;
        presentation.encodedX = 0;
        presentation.encodedY = 0;
        presentation.encodedWidth = 0;
        presentation.encodedHeight = 0;
        presentation.encodedQoi.clear();
        presentation.dirty = false;
        presentation.dirtyBaseRevision = 0;
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

        for (int column = 0; column < width; ++column)
        {
            const std::size_t sourcePixel =
                sourceOffset + static_cast<std::size_t>(column) * 4U;
            const std::size_t destinationPixel =
                destinationOffset + static_cast<std::size_t>(column) * 4U;

            const bool wasVisible =
                presentation.bgra[destinationPixel + 3U] != 0U;
            const bool isVisible =
                image[sourcePixel + 3U] != 0U;

            if (wasVisible != isVisible)
            {
                if (isVisible)
                    ++presentation.visibleAlphaPixels;
                else if (presentation.visibleAlphaPixels != 0U)
                    --presentation.visibleAlphaPixels;
            }

            std::memcpy(
                presentation.bgra.data() + destinationPixel,
                image + sourcePixel,
                4U);
        }
    }

    ++presentation.frameRevision;
    if (presentation.frameRevision == 0)
        presentation.frameRevision = 1;
    presentation.observedAt = unixSeconds();
    includeDirtyRect(
        presentation,
        static_cast<std::uint32_t>(x),
        static_cast<std::uint32_t>(y),
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height));

    // Keep an already encoded snapshot stable while SuiteBridge is reading
    // its chunks. A newer BGRA revision may arrive concurrently; the next
    // META request will encode that newer revision.
    changedSessionId = presentation.sessionId;
    changedFrameRevision = presentation.frameRevision;
    }

    publishPresentationChanged(changedSessionId, changedFrameRevision);
    return true;
}

bool VdrSuiteHbbtvPresentationStore::Read(
    VdrWebHbbtvPresentationV2& message)
{
    const VdrWebHbbtvPresentationV2 request = message;
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

    if (request.operation == VDRWEB_HBBTV_PRESENTATION_META)
    {
        // No image bytes are needed when the consumer already has the latest
        // revision. This also avoids re-encoding an idle full frame.
        if (request.frameRevision == presentation.frameRevision)
        {
            message.frameRevision = presentation.frameRevision;
            message.visibility =
                presentation.visibleAlphaPixels == 0U
                    ? VDRWEB_HBBTV_PRESENTATION_VISIBILITY_HIDDEN
                    : VDRWEB_HBBTV_PRESENTATION_VISIBILITY_VISIBLE;
            message.frameKind = VDRWEB_HBBTV_PRESENTATION_FRAME_NONE;
            message.observedAt = presentation.observedAt;
            message.baseRevision = request.frameRevision;
            message.renderWidth = presentation.renderWidth;
            message.renderHeight = presentation.renderHeight;
            message.result = VDRWEB_HBBTV_PRESENTATION_RESULT_OK;
            return true;
        }

        if (!encodeSnapshot(presentation, request.frameRevision))
        {
            message.result =
                VDRWEB_HBBTV_PRESENTATION_RESULT_FRAME_TOO_LARGE;
            return true;
        }

    }
    else if (presentation.encodedQoi.empty() ||
             request.frameRevision != presentation.encodedRevision)
    {
        message.result =
            VDRWEB_HBBTV_PRESENTATION_RESULT_REVISION_MISMATCH;
        return true;
    }

    message.frameRevision = presentation.encodedRevision;
    message.visibility =
        presentation.encodedVisibleAlphaPixels == 0U
            ? VDRWEB_HBBTV_PRESENTATION_VISIBILITY_HIDDEN
            : VDRWEB_HBBTV_PRESENTATION_VISIBILITY_VISIBLE;
    message.frameKind = presentation.encodedFrameKind;
    message.observedAt = presentation.encodedObservedAt;
    message.baseRevision = presentation.encodedBaseRevision;
    message.renderWidth = presentation.encodedRenderWidth;
    message.renderHeight = presentation.encodedRenderHeight;
    message.patchX = presentation.encodedX;
    message.patchY = presentation.encodedY;
    message.patchWidth = presentation.encodedWidth;
    message.patchHeight = presentation.encodedHeight;
    message.encodedBytes =
        static_cast<std::uint32_t>(presentation.encodedQoi.size());

    if (request.operation == VDRWEB_HBBTV_PRESENTATION_META)
    {
        message.result = VDRWEB_HBBTV_PRESENTATION_RESULT_OK;
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

