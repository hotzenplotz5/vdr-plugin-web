#include "../service/vdrsuite_hbbtv_runtime_service.h"

#include <cassert>
#include <cstring>
#include <string>
#include <type_traits>

namespace
{

void copyText(char *target, std::size_t capacity, const char *value)
{
    assert(target != nullptr);
    assert(value != nullptr);
    const std::size_t length = std::strlen(value);
    assert(length < capacity);
    std::memcpy(target, value, length + 1U);
}

VdrWebHbbtvRuntimeV1 makeRequest(
    std::uint8_t operation,
    const char *sessionId,
    const char *channelId,
    std::uint32_t applicationId,
    std::uint64_t descriptorRevision,
    std::uint8_t inputAction = VDRWEB_HBBTV_INPUT_NONE)
{
    VdrWebHbbtvRuntimeV1 request{};
    request.structSize = sizeof(request);
    request.operation = operation;
    request.inputAction = inputAction;
    request.applicationId = applicationId;
    request.descriptorRevision = descriptorRevision;
    copyText(request.sessionId, VDRWEB_HBBTV_SESSION_ID_MAX, sessionId);
    copyText(request.channelId, VDRWEB_HBBTV_CHANNEL_ID_MAX, channelId);
    return request;
}

std::uint64_t discoveryRevision(const char *channelId)
{
    VdrWebHbbtvDiscoveryV1 discovery{};
    discovery.structSize = sizeof(discovery);
    copyText(discovery.channelId, VDRWEB_HBBTV_CHANNEL_ID_MAX, channelId);
    assert(VdrSuiteHbbtvDiscoveryStore::Read(discovery));
    assert(discovery.schemaVersion == VDRWEB_HBBTV_SERVICE_SCHEMA_V1);
    assert(discovery.result == VDRWEB_HBBTV_RESULT_OK);
    assert(discovery.discoveryRevision != 0);
    return discovery.discoveryRevision;
}

}

int main()
{
    static_assert(
        std::is_standard_layout<VdrWebHbbtvRuntimeV1>::value,
        "runtime ABI must be standard layout");

    const char *channel = "C-1-1051-10301";

    VdrSuiteHbbtvDiscoveryStore::EndChannel();
    VdrSuiteHbbtvDiscoveryStore::BeginChannel(channel);
    VdrSuiteHbbtvDiscoveryStore::Upsert(
        channel,
        1,
        1,
        2,
        "HBBTV-Start",
        "https://example.invalid/",
        "start.html",
        "");

    const std::uint64_t revision = discoveryRevision(channel);

    int launchCount = 0;
    int closeCount = 0;
    int inputCount = 0;

    std::string launchedSession;
    std::string launchedChannel;
    std::string closedSession;
    std::string inputSession;
    std::string inputKey;

    VdrSuiteHbbtvRuntimeHooks hooks;
    hooks.scheduleLaunch =
        [&](const std::string& sessionId, const std::string& channelId) {
            ++launchCount;
            launchedSession = sessionId;
            launchedChannel = channelId;
            return true;
        };
    hooks.scheduleClose =
        [&](const std::string& sessionId) {
            ++closeCount;
            closedSession = sessionId;
            return true;
        };
    hooks.sendInput =
        [&](const std::string& sessionId, const std::string& key) {
            ++inputCount;
            inputSession = sessionId;
            inputKey = key;
            return true;
        };

    VdrSuiteHbbtvRuntimeService runtime(hooks);

    auto launch = makeRequest(
        VDRWEB_HBBTV_RUNTIME_LAUNCH,
        "session-a",
        channel,
        1,
        revision);

    assert(runtime.Handle(launch));
    assert(launch.schemaVersion == VDRWEB_HBBTV_RUNTIME_SCHEMA_V1);
    assert(launch.result == VDRWEB_HBBTV_RUNTIME_RESULT_ACCEPTED);
    assert(launch.state == VDRWEB_HBBTV_RUNTIME_STATE_STARTING);
    assert(launchCount == 1);
    assert(launchedSession == "session-a");
    assert(launchedChannel == channel);

    auto duplicateLaunch = makeRequest(
        VDRWEB_HBBTV_RUNTIME_LAUNCH,
        "session-a",
        channel,
        1,
        revision);

    assert(runtime.Handle(duplicateLaunch));
    assert(
        duplicateLaunch.result ==
        VDRWEB_HBBTV_RUNTIME_RESULT_ACCEPTED);
    assert(
        duplicateLaunch.state ==
        VDRWEB_HBBTV_RUNTIME_STATE_STARTING);
    assert(launchCount == 1);

    assert(runtime.CompleteLaunch("session-a", true));

    auto status = makeRequest(
        VDRWEB_HBBTV_RUNTIME_STATUS,
        "session-a",
        channel,
        1,
        revision);

    assert(runtime.Handle(status));
    assert(status.result == VDRWEB_HBBTV_RUNTIME_RESULT_OK);
    assert(status.state == VDRWEB_HBBTV_RUNTIME_STATE_ACTIVE);

    auto competingLaunch = makeRequest(
        VDRWEB_HBBTV_RUNTIME_LAUNCH,
        "session-b",
        channel,
        1,
        revision);

    assert(runtime.Handle(competingLaunch));
    assert(
        competingLaunch.result ==
        VDRWEB_HBBTV_RUNTIME_RESULT_BUSY);
    assert(launchCount == 1);

    auto inputUp = makeRequest(
        VDRWEB_HBBTV_RUNTIME_INPUT,
        "session-a",
        channel,
        1,
        revision,
        VDRWEB_HBBTV_INPUT_UP);

    assert(runtime.Handle(inputUp));
    assert(inputUp.result == VDRWEB_HBBTV_RUNTIME_RESULT_OK);
    assert(inputUp.state == VDRWEB_HBBTV_RUNTIME_STATE_ACTIVE);
    assert(inputCount == 1);
    assert(inputSession == "session-a");
    assert(inputKey == "VK_UP");

    auto inputOk = makeRequest(
        VDRWEB_HBBTV_RUNTIME_INPUT,
        "session-a",
        channel,
        1,
        revision,
        VDRWEB_HBBTV_INPUT_OK);

    assert(runtime.Handle(inputOk));
    assert(inputOk.result == VDRWEB_HBBTV_RUNTIME_RESULT_OK);
    assert(inputCount == 2);
    assert(inputKey == "VK_ENTER");

    auto unsupportedInput = makeRequest(
        VDRWEB_HBBTV_RUNTIME_INPUT,
        "session-a",
        channel,
        1,
        revision,
        255);

    assert(runtime.Handle(unsupportedInput));
    assert(
        unsupportedInput.result ==
        VDRWEB_HBBTV_RUNTIME_RESULT_ACTION_UNSUPPORTED);
    assert(inputCount == 2);

    VdrSuiteHbbtvDiscoveryStore::Upsert(
        channel,
        2,
        2,
        5,
        "Second application",
        "https://example.invalid/",
        "second.html",
        "");

    const std::uint64_t newerRevision = discoveryRevision(channel);
    assert(newerRevision > revision);

    auto staleInput = makeRequest(
        VDRWEB_HBBTV_RUNTIME_INPUT,
        "session-a",
        channel,
        1,
        revision,
        VDRWEB_HBBTV_INPUT_LEFT);

    assert(runtime.Handle(staleInput));
    assert(
        staleInput.result ==
        VDRWEB_HBBTV_RUNTIME_RESULT_DISCOVERY_STALE);
    assert(inputCount == 2);

    auto close = makeRequest(
        VDRWEB_HBBTV_RUNTIME_CLOSE,
        "session-a",
        channel,
        1,
        revision);

    assert(runtime.Handle(close));
    assert(close.result == VDRWEB_HBBTV_RUNTIME_RESULT_ACCEPTED);
    assert(close.state == VDRWEB_HBBTV_RUNTIME_STATE_CLOSING);
    assert(closeCount == 1);
    assert(closedSession == "session-a");
    assert(runtime.CompleteClose("session-a", true));

    auto afterClose = makeRequest(
        VDRWEB_HBBTV_RUNTIME_INPUT,
        "session-a",
        channel,
        1,
        revision,
        VDRWEB_HBBTV_INPUT_UP);

    assert(runtime.Handle(afterClose));
    assert(
        afterClose.result ==
        VDRWEB_HBBTV_RUNTIME_RESULT_SESSION_NOT_ACTIVE);
    assert(inputCount == 2);

    auto staleLaunch = makeRequest(
        VDRWEB_HBBTV_RUNTIME_LAUNCH,
        "session-c",
        channel,
        1,
        revision);

    assert(runtime.Handle(staleLaunch));
    assert(
        staleLaunch.result ==
        VDRWEB_HBBTV_RUNTIME_RESULT_DISCOVERY_STALE);
    assert(launchCount == 1);

    auto nonLaunchable = makeRequest(
        VDRWEB_HBBTV_RUNTIME_LAUNCH,
        "session-d",
        channel,
        2,
        newerRevision);

    assert(runtime.Handle(nonLaunchable));
    assert(
        nonLaunchable.result ==
        VDRWEB_HBBTV_RUNTIME_RESULT_APPLICATION_NOT_LAUNCHABLE);
    assert(launchCount == 1);

    VdrSuiteHbbtvDiscoveryStore::Upsert(
        channel,
        3,
        1,
        3,
        "Ambiguous start application",
        "https://example.invalid/",
        "other-start.html",
        "");

    const std::uint64_t ambiguousRevision = discoveryRevision(channel);

    auto ambiguousLaunch = makeRequest(
        VDRWEB_HBBTV_RUNTIME_LAUNCH,
        "session-e",
        channel,
        1,
        ambiguousRevision);

    assert(runtime.Handle(ambiguousLaunch));
    assert(
        ambiguousLaunch.result ==
        VDRWEB_HBBTV_RUNTIME_RESULT_APPLICATION_NOT_LAUNCHABLE);
    assert(launchCount == 1);

    VdrSuiteHbbtvDiscoveryStore::EndChannel();
    return 0;
}
