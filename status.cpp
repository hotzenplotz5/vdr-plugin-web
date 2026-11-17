/*
 * status.c: keep track of VDR device status
 *
 * See the README file for copyright information and how to reach the author.
 *
 * $Id$
 */

#include <vdr/channels.h>
#include <string>
#include <vector>
#include <algorithm>
#include "web.h"
#include "status.h"
#include "ait.h"
#include "BrowserClient.h"
#include "webosdpage.h"
#include "jsonescape.h"
#include "service/vdrsuite_hbbtv_discovery_service.h"
#include "streamevent.h"

const char *channelJson = R"(
    {
        "channelId": "%s",
        "channelType": %d,
        "ccid": "ccid://1.0",
        "nid": %d,
        "dsd": "",
        "onid": %d,
        "tsid": %d,
        "sid": %d,
        "name": "%s",
        "longName": "%s",
        "description": "OIPF (SD&amp;S) - TCServiceData doesn’t support yet!",
        "authorised": true,
        "genre": null,
        "hidden": false,
        "idType": "%d",
        "channelMaxBitRate": 0,
        "manualBlock": false,
        "majorChannel": 1,
        "ipBroadcastID": "rtp://1.2.3.4/",
        "locked": false
    }
)";

// Audio/subtitle components are ordered by PID, matching VideoPlayer::SelectTrack.
static std::string channelComponentsJson(const cChannel* channel) {
    struct Track {
        int pid;
        std::string json;
    };

    std::vector<Track> audio;
    for (int i = 0; channel->Apid(i); ++i) {
        const int type = channel->Atype(i);
        const char *encoding =
            (type == 0x0F || type == 0x11) ? "audio/mp4" : "audio/mpeg";
        audio.push_back({
            channel->Apid(i),
            *cString::sprintf(
                "{\"pid\":%d,\"language\":\"%s\",\"encoding\":\"%s\"}",
                channel->Apid(i),
                jsonEscape(channel->Alang(i)).c_str(),
                encoding)
        });
    }

    for (int i = 0; channel->Dpid(i); ++i) {
        audio.push_back({
            channel->Dpid(i),
            *cString::sprintf(
                "{\"pid\":%d,\"language\":\"%s\",\"encoding\":\"audio/ac3\"}",
                channel->Dpid(i),
                jsonEscape(channel->Dlang(i)).c_str())
        });
    }

    std::vector<Track> subtitles;
    for (int i = 0; channel->Spid(i); ++i) {
        const bool hearingImpaired =
            channel->SubtitlingType(i) >= 0x20 &&
            channel->SubtitlingType(i) <= 0x25;
        subtitles.push_back({
            channel->Spid(i),
            *cString::sprintf(
                "{\"pid\":%d,\"language\":\"%s\",\"hearingImpaired\":%s}",
                channel->Spid(i),
                jsonEscape(channel->Slang(i)).c_str(),
                hearingImpaired ? "true" : "false")
        });
    }

    auto byPid = [](const Track& a, const Track& b) {
        return a.pid < b.pid;
    };
    std::sort(audio.begin(), audio.end(), byPid);
    std::sort(subtitles.begin(), subtitles.end(), byPid);

    std::string result = "{\"audio\":[";
    for (size_t i = 0; i < audio.size(); ++i)
        result += (i ? "," : "") + audio[i].json;

    result += "],\"subtitle\":[";
    for (size_t i = 0; i < subtitles.size(); ++i)
        result += (i ? "," : "") + subtitles[i].json;

    result += "]}";
    return result;
}

static std::string programmeJson(const cEvent *event) {
    if (event == nullptr)
        return "";

    std::string ratings = "[]";
    if (event->ParentalRating() > 0) {
        ratings = *cString::sprintf(
            "[{\"name\":\"%d\",\"scheme\":\"dvb-si\",\"value\":%d,\"region\":\"de\"}]",
            event->ParentalRating(),
            event->ParentalRating() - 3);
    }

    return *cString::sprintf(
        "{\"programmeID\":\"%u\",\"name\":\"%s\","
        "\"description\":\"%s\",\"longDescription\":\"%s\","
        "\"startTime\":%ld,\"duration\":%d,\"parentalRatings\":%s}",
        event->EventID(),
        jsonEscape(event->Title()).c_str(),
        jsonEscape(event->ShortText()).c_str(),
        jsonEscape(event->Description()).c_str(),
        static_cast<long>(event->StartTime()),
        event->Duration(),
        ratings.c_str());
}

static std::string channelProgrammesJson(
        const cChannel* channel,
        tEventID *presentId = nullptr) {
    std::string result = "[";

    LOCK_SCHEDULES_READ
    const cSchedule *schedule = Schedules->GetSchedule(channel);
    if (schedule != nullptr) {
        const cEvent *present = schedule->GetPresentEvent();
        const cEvent *following = schedule->GetFollowingEvent();

        if (presentId != nullptr)
            *presentId = present ? present->EventID() : 0;

        const std::string p = programmeJson(present);
        const std::string f = programmeJson(following);
        result += p;
        if (!p.empty() && !f.empty())
            result += ",";
        result += f;
    }

    return result + "]";
}

inline const char* toChannelJson(const cChannel* channel) {
    // longName, Name => currentChannel
    // nid            => ??? (use 1 as default)
    // onid           => channel, Nid
    // sid            => channel, Sid
    // tsid           => channel, Tid
    // channelType    => HDTV 0x19, TV 0x01, Radio 0x02
    // idType         => ??? (use 15 as default)
    int channelType;

    if (strstr(channel->Name(), "HD") != nullptr) {
        channelType = 0x19;
    } else if (channel->Rid() > 0) {
        channelType = 0x02;
    } else {
        channelType = 0x01;
    }

    char *cmd;
    const std::string name = jsonEscape(channel->Name());
    asprintf(&cmd, channelJson,
             *channel->GetChannelID().ToString(),
             channelType,
             1,
             channel->Nid(),
             channel->Tid(),
             channel->Sid(),
             name.c_str(),
             name.c_str(),
             15);

    std::string json = cmd;
    free(cmd);

    const auto end = json.find_last_of('}');
    if (end != std::string::npos) {
        json = json.substr(0, end) +
               ",\n        \"components\": " +
               channelComponentsJson(channel) +
               ",\n        \"programmes\": " +
               channelProgrammesJson(channel) +
               "\n    }\n";
    }

    return strdup(json.c_str());
}

cHbbtvDeviceStatus::cHbbtvDeviceStatus() {
   device = nullptr;
   aitFilter = nullptr;
   sid = -1;
}

cHbbtvDeviceStatus::~cHbbtvDeviceStatus() {
}

void cHbbtvDeviceStatus::ChannelSwitch(const cDevice * vdrDevice, int channelNumber, bool LiveView) {
   // Indicates a channel switch on the given DVB device.
   // If ChannelNumber is 0, this is before the channel is being switched,
   // otherwise ChannelNumber is the number of the channel that has been switched to.
   // LiveView tells whether this channel switch is for live viewing.
   if (LiveView) {
      // Stream-event filters are scoped to the live channel.
      StreamEventsChannelSwitch(nullptr);

      if (device) {
         isyslog("[vdrweb] Detaching HbbTV ait filter from device %d",
                 device->CardIndex() + 1);
         device->Detach(aitFilter);
         delete aitFilter;
         device = nullptr;
         aitFilter = nullptr;
         sid = -1;
         // ChannelNumber == 0 is VDR's pre-switch notification.
         // Keep the current HbbTV discovery until the concrete
         // target channel is known below.
      }

      if (channelNumber) {
         device = cDevice::ActualDevice();

         std::string channelId;
         std::string json;
         {
#if APIVERSNUM >= 20301
            LOCK_CHANNELS_READ
            auto channel = Channels->GetByNumber(channelNumber);
#else
            auto channel = Channels->GetByNumber(channelNumber);
#endif
            if (channel == nullptr) {
               device = nullptr;
               return;
            }

            sid = channel->Sid();
            lastPresentEventId = 0;
            const cString id = channel->GetChannelID().ToString();
            if (*id == nullptr) {
               device = nullptr;
               sid = -1;
               return;
            }

            channelId = *id;
            const char* buffer = toChannelJson(channel);
            json = buffer;
            free((void *) buffer);
         }

         VdrSuiteHbbtvDiscoveryStore::BeginChannel(channelId.c_str());

         // Browser IPC must not hold VDR's channel lock.
         if (browserClient != nullptr)
            browserClient->InsertChannel(json);

         if (device == nullptr)
            return;

         device->AttachFilter(aitFilter = new cAitFilter(sid));
         StreamEventsChannelSwitch(device);
         isyslog("[vdrweb] Attached HbbTV ait filter to device %d, vdrDev=%d actDev=%d, Sid=0x%04x",
                 device->CardIndex()+1,
                 vdrDevice ? vdrDevice->CardIndex()+1 : -1,
                 cDevice::ActualDevice() ? cDevice::ActualDevice()->CardIndex()+1 : -1,
                 sid);
      }
   }
}

void cHbbtvDeviceStatus::CheckProgramme() {
    if (sid < 0 || browserClient == nullptr)
        return;

    std::string json;
    {
        LOCK_CHANNELS_READ
        const cChannel *channel =
            Channels->GetByNumber(cDevice::CurrentChannel());
        if (channel == nullptr)
            return;

        tEventID presentId = 0;
        channelProgrammesJson(channel, &presentId);

        if (presentId == lastPresentEventId)
            return;

        lastPresentEventId = presentId;

        const char *buffer = toChannelJson(channel);
        json = buffer;
        free((void *)buffer);
    }

    // Browser IPC stays outside the channel/schedule lock.
    browserClient->InsertChannel(json);
}

void cHbbtvDeviceStatus::SetVolume(int Volume, bool Absolute) {
    // instead using the parameter (relative volume change), use the absolute volume of the Device

   volume = Absolute ? Volume : volume + Volume;
   WebOSDPage* page = WebOSDPage::Get();
   if (page)
       page->DrawVolume(volume);
}
