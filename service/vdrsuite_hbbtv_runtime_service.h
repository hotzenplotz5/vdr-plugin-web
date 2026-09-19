#pragma once

#include "vdrsuite_hbbtv_discovery_service.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

#define VDRWEB_SERVICE_HBBTV_RUNTIME_V1 "VdrWeb::HbbtvRuntime-v1"
#define VDRWEB_HBBTV_RUNTIME_SCHEMA_V1 1U
#define VDRWEB_HBBTV_SESSION_ID_MAX 128U

enum VdrWebHbbtvRuntimeOperationV1 : std::uint8_t {
    VDRWEB_HBBTV_RUNTIME_LAUNCH = 1,
    VDRWEB_HBBTV_RUNTIME_STATUS = 2,
    VDRWEB_HBBTV_RUNTIME_INPUT = 3,
    VDRWEB_HBBTV_RUNTIME_CLOSE = 4
};

enum VdrWebHbbtvRuntimeResultV1 : std::uint8_t {
    VDRWEB_HBBTV_RUNTIME_RESULT_OK = 0,
    VDRWEB_HBBTV_RUNTIME_RESULT_ACCEPTED = 1,
    VDRWEB_HBBTV_RUNTIME_RESULT_INVALID_REQUEST = 2,
    VDRWEB_HBBTV_RUNTIME_RESULT_DISCOVERY_STALE = 3,
    VDRWEB_HBBTV_RUNTIME_RESULT_APPLICATION_NOT_LAUNCHABLE = 4,
    VDRWEB_HBBTV_RUNTIME_RESULT_BUSY = 5,
    VDRWEB_HBBTV_RUNTIME_RESULT_SESSION_NOT_ACTIVE = 6,
    VDRWEB_HBBTV_RUNTIME_RESULT_ACTION_UNSUPPORTED = 7,
    VDRWEB_HBBTV_RUNTIME_RESULT_RUNTIME_UNAVAILABLE = 8
};

enum VdrWebHbbtvRuntimeStateV1 : std::uint8_t {
    VDRWEB_HBBTV_RUNTIME_STATE_NONE = 0,
    VDRWEB_HBBTV_RUNTIME_STATE_STARTING = 1,
    VDRWEB_HBBTV_RUNTIME_STATE_ACTIVE = 2,
    VDRWEB_HBBTV_RUNTIME_STATE_CLOSING = 3,
    VDRWEB_HBBTV_RUNTIME_STATE_FAILED = 4
};

enum VdrWebHbbtvInputActionV1 : std::uint8_t {
    VDRWEB_HBBTV_INPUT_NONE = 0,
    VDRWEB_HBBTV_INPUT_UP = 1,
    VDRWEB_HBBTV_INPUT_DOWN = 2,
    VDRWEB_HBBTV_INPUT_LEFT = 3,
    VDRWEB_HBBTV_INPUT_RIGHT = 4,
    VDRWEB_HBBTV_INPUT_OK = 5,
    VDRWEB_HBBTV_INPUT_BACK = 6,
    VDRWEB_HBBTV_INPUT_RED = 7,
    VDRWEB_HBBTV_INPUT_GREEN = 8,
    VDRWEB_HBBTV_INPUT_YELLOW = 9,
    VDRWEB_HBBTV_INPUT_BLUE = 10,
    VDRWEB_HBBTV_INPUT_0 = 11,
    VDRWEB_HBBTV_INPUT_1 = 12,
    VDRWEB_HBBTV_INPUT_2 = 13,
    VDRWEB_HBBTV_INPUT_3 = 14,
    VDRWEB_HBBTV_INPUT_4 = 15,
    VDRWEB_HBBTV_INPUT_5 = 16,
    VDRWEB_HBBTV_INPUT_6 = 17,
    VDRWEB_HBBTV_INPUT_7 = 18,
    VDRWEB_HBBTV_INPUT_8 = 19,
    VDRWEB_HBBTV_INPUT_9 = 20,
    VDRWEB_HBBTV_INPUT_PLAY = 21,
    VDRWEB_HBBTV_INPUT_PAUSE = 22,
    VDRWEB_HBBTV_INPUT_STOP = 23,
    VDRWEB_HBBTV_INPUT_FAST_FORWARD = 24,
    VDRWEB_HBBTV_INPUT_REWIND = 25
};

struct VdrWebHbbtvRuntimeV1 {
    std::uint32_t structSize;

    // Request.
    std::uint8_t operation;
    std::uint8_t inputAction;
    std::uint16_t reservedRequest;
    std::uint32_t applicationId;
    std::uint64_t descriptorRevision;
    char sessionId[VDRWEB_HBBTV_SESSION_ID_MAX];
    char channelId[VDRWEB_HBBTV_CHANNEL_ID_MAX];

    // Response.
    std::uint32_t schemaVersion;
    std::uint8_t result;
    std::uint8_t state;
    std::uint16_t reservedResponse;
};

struct VdrSuiteHbbtvRuntimeHooks {
    std::function<bool(const std::string&, const std::string&)> scheduleLaunch;
    std::function<bool(const std::string&)> scheduleClose;
    std::function<bool(const std::string&, const std::string&)> sendInput;
};

class VdrSuiteHbbtvRuntimeService final {
public:
    explicit VdrSuiteHbbtvRuntimeService(VdrSuiteHbbtvRuntimeHooks hooks)
        : hooks_(std::move(hooks))
    {
    }

    bool Handle(VdrWebHbbtvRuntimeV1& message)
    {
        const VdrWebHbbtvRuntimeV1 request = message;
        resetResponse(message, request);

        std::string sessionId;
        std::string channelId;
        if (!validRequest(request, sessionId, channelId)) {
            message.result = VDRWEB_HBBTV_RUNTIME_RESULT_INVALID_REQUEST;
            return true;
        }

        switch (request.operation) {
        case VDRWEB_HBBTV_RUNTIME_LAUNCH:
            return handleLaunch(request, sessionId, channelId, message);
        case VDRWEB_HBBTV_RUNTIME_STATUS:
            return handleStatus(request, sessionId, channelId, message);
        case VDRWEB_HBBTV_RUNTIME_INPUT:
            return handleInput(request, sessionId, channelId, message);
        case VDRWEB_HBBTV_RUNTIME_CLOSE:
            return handleClose(request, sessionId, channelId, message);
        default:
            message.result = VDRWEB_HBBTV_RUNTIME_RESULT_INVALID_REQUEST;
            return true;
        }
    }

    bool CompleteLaunch(const std::string& sessionId, bool success)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_.owned || active_.sessionId != sessionId ||
            active_.state != VDRWEB_HBBTV_RUNTIME_STATE_STARTING) {
            return false;
        }

        active_.state = success
            ? VDRWEB_HBBTV_RUNTIME_STATE_ACTIVE
            : VDRWEB_HBBTV_RUNTIME_STATE_FAILED;
        return true;
    }

    bool CompleteClose(const std::string& sessionId, bool success)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_.owned || active_.sessionId != sessionId ||
            active_.state != VDRWEB_HBBTV_RUNTIME_STATE_CLOSING) {
            return false;
        }

        if (success) {
            active_ = {};
        }
        else {
            active_.state = VDRWEB_HBBTV_RUNTIME_STATE_FAILED;
        }
        return true;
    }

private:
    struct ActiveSession {
        bool owned = false;
        std::string sessionId;
        std::string channelId;
        std::uint32_t applicationId = 0;
        std::uint64_t descriptorRevision = 0;
        std::uint8_t state = VDRWEB_HBBTV_RUNTIME_STATE_NONE;
    };

    static bool boundedText(
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
    static void copyText(char (&target)[N], const char* source)
    {
        std::memset(target, 0, N);
        if (source == nullptr) return;
        std::size_t length = 0;
        while (length + 1U < N && source[length] != '\0') ++length;
        std::memcpy(target, source, length);
        target[length] = '\0';
    }

    static void resetResponse(
        VdrWebHbbtvRuntimeV1& response,
        const VdrWebHbbtvRuntimeV1& request)
    {
        std::memset(&response, 0, sizeof(response));
        response.structSize = sizeof(response);
        response.operation = request.operation;
        response.inputAction = request.inputAction;
        response.applicationId = request.applicationId;
        response.descriptorRevision = request.descriptorRevision;
        copyText(response.sessionId, request.sessionId);
        copyText(response.channelId, request.channelId);
        response.schemaVersion = VDRWEB_HBBTV_RUNTIME_SCHEMA_V1;
        response.result = VDRWEB_HBBTV_RUNTIME_RESULT_INVALID_REQUEST;
        response.state = VDRWEB_HBBTV_RUNTIME_STATE_NONE;
    }

    static bool validRequest(
        const VdrWebHbbtvRuntimeV1& request,
        std::string& sessionId,
        std::string& channelId)
    {
        if (request.structSize != sizeof(request) ||
            request.applicationId == 0 ||
            request.descriptorRevision == 0 ||
            !boundedText(request.sessionId, VDRWEB_HBBTV_SESSION_ID_MAX, sessionId) ||
            !boundedText(request.channelId, VDRWEB_HBBTV_CHANNEL_ID_MAX, channelId)) {
            return false;
        }
        return true;
    }

    static bool sameContext(
        const ActiveSession& active,
        const VdrWebHbbtvRuntimeV1& request,
        const std::string& sessionId,
        const std::string& channelId)
    {
        return active.owned &&
            active.sessionId == sessionId &&
            active.channelId == channelId &&
            active.applicationId == request.applicationId &&
            active.descriptorRevision == request.descriptorRevision;
    }

    static std::uint8_t currentDiscovery(
        const std::string& channelId,
        std::uint64_t descriptorRevision,
        std::uint32_t applicationId,
        bool requireLaunchable)
    {
        VdrWebHbbtvDiscoveryV1 discovery{};
        discovery.structSize = sizeof(discovery);
        if (channelId.size() >= VDRWEB_HBBTV_CHANNEL_ID_MAX)
            return VDRWEB_HBBTV_RUNTIME_RESULT_INVALID_REQUEST;
        std::memcpy(discovery.channelId, channelId.data(), channelId.size());
        discovery.channelId[channelId.size()] = '\0';

        if (!VdrSuiteHbbtvDiscoveryStore::Read(discovery) ||
            discovery.schemaVersion != VDRWEB_HBBTV_SERVICE_SCHEMA_V1 ||
            discovery.result != VDRWEB_HBBTV_RESULT_OK ||
            discovery.receiverActive == 0 ||
            discovery.discoveryRevision != descriptorRevision) {
            return VDRWEB_HBBTV_RUNTIME_RESULT_DISCOVERY_STALE;
        }

        bool requestedPresent = false;
        std::size_t launchableCount = 0;
        bool requestedLaunchable = false;
        for (std::size_t index = 0; index < discovery.applicationCount; ++index) {
            const VdrWebHbbtvApplicationV1& application =
                discovery.applications[index];
            if (application.applicationId == applicationId)
                requestedPresent = true;
            if (application.controlCode == 1U) {
                ++launchableCount;
                if (application.applicationId == applicationId)
                    requestedLaunchable = true;
            }
        }

        if (!requestedPresent)
            return VDRWEB_HBBTV_RUNTIME_RESULT_DISCOVERY_STALE;

        if (requireLaunchable &&
            (!requestedLaunchable || launchableCount != 1U)) {
            return VDRWEB_HBBTV_RUNTIME_RESULT_APPLICATION_NOT_LAUNCHABLE;
        }

        return VDRWEB_HBBTV_RUNTIME_RESULT_OK;
    }

    static const char* inputKey(std::uint8_t action)
    {
        switch (action) {
        case VDRWEB_HBBTV_INPUT_UP: return "VK_UP";
        case VDRWEB_HBBTV_INPUT_DOWN: return "VK_DOWN";
        case VDRWEB_HBBTV_INPUT_LEFT: return "VK_LEFT";
        case VDRWEB_HBBTV_INPUT_RIGHT: return "VK_RIGHT";
        case VDRWEB_HBBTV_INPUT_OK: return "VK_ENTER";
        case VDRWEB_HBBTV_INPUT_BACK: return "VK_BACK";
        case VDRWEB_HBBTV_INPUT_RED: return "VK_RED";
        case VDRWEB_HBBTV_INPUT_GREEN: return "VK_GREEN";
        case VDRWEB_HBBTV_INPUT_YELLOW: return "VK_YELLOW";
        case VDRWEB_HBBTV_INPUT_BLUE: return "VK_BLUE";
        case VDRWEB_HBBTV_INPUT_0: return "VK_0";
        case VDRWEB_HBBTV_INPUT_1: return "VK_1";
        case VDRWEB_HBBTV_INPUT_2: return "VK_2";
        case VDRWEB_HBBTV_INPUT_3: return "VK_3";
        case VDRWEB_HBBTV_INPUT_4: return "VK_4";
        case VDRWEB_HBBTV_INPUT_5: return "VK_5";
        case VDRWEB_HBBTV_INPUT_6: return "VK_6";
        case VDRWEB_HBBTV_INPUT_7: return "VK_7";
        case VDRWEB_HBBTV_INPUT_8: return "VK_8";
        case VDRWEB_HBBTV_INPUT_9: return "VK_9";
        case VDRWEB_HBBTV_INPUT_PLAY: return "VK_PLAY";
        case VDRWEB_HBBTV_INPUT_PAUSE: return "VK_PAUSE";
        case VDRWEB_HBBTV_INPUT_STOP: return "VK_STOP";
        case VDRWEB_HBBTV_INPUT_FAST_FORWARD: return "VK_FAST_FWD";
        case VDRWEB_HBBTV_INPUT_REWIND: return "VK_REWIND";
        default: return nullptr;
        }
    }

    bool handleLaunch(
        const VdrWebHbbtvRuntimeV1& request,
        const std::string& sessionId,
        const std::string& channelId,
        VdrWebHbbtvRuntimeV1& response)
    {
        const std::uint8_t discoveryResult = currentDiscovery(
            channelId,
            request.descriptorRevision,
            request.applicationId,
            true);
        if (discoveryResult != VDRWEB_HBBTV_RUNTIME_RESULT_OK) {
            response.result = discoveryResult;
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.owned) {
                if (sameContext(active_, request, sessionId, channelId) &&
                    (active_.state == VDRWEB_HBBTV_RUNTIME_STATE_STARTING ||
                     active_.state == VDRWEB_HBBTV_RUNTIME_STATE_ACTIVE)) {
                    response.result = VDRWEB_HBBTV_RUNTIME_RESULT_ACCEPTED;
                    response.state = active_.state;
                    return true;
                }
                response.result = VDRWEB_HBBTV_RUNTIME_RESULT_BUSY;
                response.state = active_.state;
                return true;
            }

            active_.owned = true;
            active_.sessionId = sessionId;
            active_.channelId = channelId;
            active_.applicationId = request.applicationId;
            active_.descriptorRevision = request.descriptorRevision;
            active_.state = VDRWEB_HBBTV_RUNTIME_STATE_STARTING;
        }

        if (!hooks_.scheduleLaunch || !hooks_.scheduleLaunch(sessionId, channelId)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.owned && active_.sessionId == sessionId)
                active_.state = VDRWEB_HBBTV_RUNTIME_STATE_FAILED;
            response.result = VDRWEB_HBBTV_RUNTIME_RESULT_RUNTIME_UNAVAILABLE;
            response.state = VDRWEB_HBBTV_RUNTIME_STATE_FAILED;
            return true;
        }

        response.result = VDRWEB_HBBTV_RUNTIME_RESULT_ACCEPTED;
        response.state = VDRWEB_HBBTV_RUNTIME_STATE_STARTING;
        return true;
    }

    bool handleStatus(
        const VdrWebHbbtvRuntimeV1& request,
        const std::string& sessionId,
        const std::string& channelId,
        VdrWebHbbtvRuntimeV1& response)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sameContext(active_, request, sessionId, channelId)) {
            response.result = VDRWEB_HBBTV_RUNTIME_RESULT_SESSION_NOT_ACTIVE;
            return true;
        }
        response.result = VDRWEB_HBBTV_RUNTIME_RESULT_OK;
        response.state = active_.state;
        return true;
    }

    bool handleInput(
        const VdrWebHbbtvRuntimeV1& request,
        const std::string& sessionId,
        const std::string& channelId,
        VdrWebHbbtvRuntimeV1& response)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!sameContext(active_, request, sessionId, channelId) ||
                active_.state != VDRWEB_HBBTV_RUNTIME_STATE_ACTIVE) {
                response.result = VDRWEB_HBBTV_RUNTIME_RESULT_SESSION_NOT_ACTIVE;
                return true;
            }
        }

        const std::uint8_t discoveryResult = currentDiscovery(
            channelId,
            request.descriptorRevision,
            request.applicationId,
            false);
        if (discoveryResult != VDRWEB_HBBTV_RUNTIME_RESULT_OK) {
            response.result = discoveryResult;
            response.state = VDRWEB_HBBTV_RUNTIME_STATE_ACTIVE;
            return true;
        }

        const char* key = inputKey(request.inputAction);
        if (key == nullptr) {
            response.result = VDRWEB_HBBTV_RUNTIME_RESULT_ACTION_UNSUPPORTED;
            response.state = VDRWEB_HBBTV_RUNTIME_STATE_ACTIVE;
            return true;
        }

        if (!hooks_.sendInput || !hooks_.sendInput(sessionId, key)) {
            response.result = VDRWEB_HBBTV_RUNTIME_RESULT_RUNTIME_UNAVAILABLE;
            response.state = VDRWEB_HBBTV_RUNTIME_STATE_ACTIVE;
            return true;
        }

        response.result = VDRWEB_HBBTV_RUNTIME_RESULT_OK;
        response.state = VDRWEB_HBBTV_RUNTIME_STATE_ACTIVE;
        return true;
    }

    bool handleClose(
        const VdrWebHbbtvRuntimeV1& request,
        const std::string& sessionId,
        const std::string& channelId,
        VdrWebHbbtvRuntimeV1& response)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!sameContext(active_, request, sessionId, channelId)) {
                response.result = VDRWEB_HBBTV_RUNTIME_RESULT_SESSION_NOT_ACTIVE;
                return true;
            }
            if (active_.state == VDRWEB_HBBTV_RUNTIME_STATE_CLOSING) {
                response.result = VDRWEB_HBBTV_RUNTIME_RESULT_ACCEPTED;
                response.state = active_.state;
                return true;
            }
            active_.state = VDRWEB_HBBTV_RUNTIME_STATE_CLOSING;
        }

        if (!hooks_.scheduleClose || !hooks_.scheduleClose(sessionId)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.owned && active_.sessionId == sessionId)
                active_.state = VDRWEB_HBBTV_RUNTIME_STATE_FAILED;
            response.result = VDRWEB_HBBTV_RUNTIME_RESULT_RUNTIME_UNAVAILABLE;
            response.state = VDRWEB_HBBTV_RUNTIME_STATE_FAILED;
            return true;
        }

        response.result = VDRWEB_HBBTV_RUNTIME_RESULT_ACCEPTED;
        response.state = VDRWEB_HBBTV_RUNTIME_STATE_CLOSING;
        return true;
    }

    VdrSuiteHbbtvRuntimeHooks hooks_;
    std::mutex mutex_;
    ActiveSession active_;
};
