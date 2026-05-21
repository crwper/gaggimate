#include "RemoteSyncPlugin.h"

#include "../core/Controller.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <vector>

namespace {

constexpr TickType_t SYNC_INTERVAL_TICKS = pdMS_TO_TICKS(5 * 60 * 1000);
constexpr unsigned long STARTUP_DELAY_MS = 15000;
constexpr int HTTP_TIMEOUT_MS = 10000;
constexpr const char *SHOTS_DIR = "/h";

struct ManifestEntry {
    String id;
    int64_t slogSize = 0;
};

String trimTrailingSlash(const String &s) {
    if (s.length() > 0 && s.charAt(s.length() - 1) == '/') {
        return s.substring(0, s.length() - 1);
    }
    return s;
}

void addAuth(HTTPClient &http, const String &token) {
    http.addHeader("Authorization", String("Bearer ") + token);
}

// Walks /h/ and returns one entry per `<id>.slog` file. The plugin treats
// shot data as upload-only and atomic: a file appearing in /h/ is considered
// complete because `ShotHistoryPlugin` only renames/closes after the final
// header patch. Server-side header validation is the authoritative atomicity
// guard if a partial somehow gets uploaded; see the server's POST handler.
std::vector<ManifestEntry> readLocalManifest(FS *fs) {
    std::vector<ManifestEntry> out;
    File dir = fs->open(SHOTS_DIR);
    if (!dir || !dir.isDirectory()) {
        return out;
    }

    File f = dir.openNextFile();
    while (f) {
        String name = f.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0)
            name = name.substring(slash + 1);

        if (name.endsWith(".slog")) {
            String id = name.substring(0, name.length() - 5);
            out.push_back({id, static_cast<int64_t>(f.size())});
        }
        f = dir.openNextFile();
    }
    dir.close();
    return out;
}

bool fetchServerManifest(const String &baseUrl, const String &token, std::vector<ManifestEntry> &out) {
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(baseUrl + "/api/manifest")) {
        return false;
    }
    addAuth(http, token);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        ESP_LOGW("RemoteSyncPlugin", "manifest GET %d", code);
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        return false;
    }
    for (JsonObject obj : doc.as<JsonArray>()) {
        ManifestEntry e;
        e.id = obj["id"].as<String>();
        e.slogSize = obj["slog_size"].as<int64_t>();
        out.push_back(e);
    }
    return true;
}

bool postFile(const String &url, const String &token, const String &contentType, FS *fs, const String &fsPath) {
    File f = fs->open(fsPath);
    if (!f)
        return false;
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(url)) {
        f.close();
        return false;
    }
    addAuth(http, token);
    http.addHeader("Content-Type", contentType);
    int code = http.sendRequest("POST", &f, f.size());
    f.close();
    http.end();
    return code >= 200 && code < 300;
}

const ManifestEntry *findById(const std::vector<ManifestEntry> &v, const String &id) {
    for (auto &e : v)
        if (e.id == id)
            return &e;
    return nullptr;
}

} // namespace

void RemoteSyncPlugin::setup(Controller *c, PluginManager *pm) {
    controller = c;
    pluginManager = pm;
    fs = controller->isSDCard() ? static_cast<FS *>(&SD_MMC) : static_cast<FS *>(&SPIFFS);

    pluginManager->on("controller:wifi:connect", [this](Event const &event) {
        // AP-mode connects can't reach an external server.
        if (event.getInt("AP") == 0) {
            requestSync();
        }
    });
    // Fires after the .slog file is closed and the index entry is written
    // (ShotHistoryPlugin::record). At that point the file is final and safe
    // to upload.
    pluginManager->on("history:shot:save", [this](Event const &) { requestSync(); });

    xTaskCreatePinnedToCore(taskTrampoline, "RemoteSync", configMINIMAL_STACK_SIZE * 8, this, 1, &taskHandle, 0);
}

void RemoteSyncPlugin::requestSync() {
    if (taskHandle != nullptr) {
        xTaskNotifyGive(taskHandle);
    }
}

void RemoteSyncPlugin::taskTrampoline(void *arg) { static_cast<RemoteSyncPlugin *>(arg)->taskLoop(); }

void RemoteSyncPlugin::taskLoop() {
    vTaskDelay(pdMS_TO_TICKS(STARTUP_DELAY_MS));
    ESP_LOGI(LOG_TAG, "Task started");
    while (true) {
        const Settings &s = controller->getSettings();
        const bool enabled = s.isRemoteSyncEnabled();
        const bool urlSet = !s.getRemoteSyncUrl().isEmpty();
        const bool tokenSet = !s.getRemoteSyncToken().isEmpty();
        const bool wifiUp = WiFi.status() == WL_CONNECTED;
        if (enabled && urlSet && tokenSet && wifiUp) {
            runSync();
        } else {
            ESP_LOGI(LOG_TAG, "Skipping sync (enabled=%d url_set=%d token_set=%d wifi=%d)",
                     enabled, urlSet, tokenSet, wifiUp);
        }
        // Either the periodic timer fires or someone calls requestSync().
        ulTaskNotifyTake(pdTRUE, SYNC_INTERVAL_TICKS);
    }
}

bool RemoteSyncPlugin::runSync() {
    const Settings &s = controller->getSettings();
    const String baseUrl = trimTrailingSlash(s.getRemoteSyncUrl());
    const String token = s.getRemoteSyncToken();

    std::vector<ManifestEntry> server;
    if (!fetchServerManifest(baseUrl, token, server)) {
        ESP_LOGW(LOG_TAG, "Sync round skipped: server unreachable");
        return false;
    }
    std::vector<ManifestEntry> local = readLocalManifest(fs);

    int uploaded = 0;

    // Local → server: upload any shot the server doesn't already have, or
    // whose recorded size differs from local. Server-only entries are left
    // alone — the server is a downstream consumer with its own retention,
    // so we never delete from absence.
    for (auto &le : local) {
        const ManifestEntry *se = findById(server, le.id);
        const bool needSlog = !se || se->slogSize != le.slogSize;
        if (!needSlog) continue;

        const String url = baseUrl + "/api/shots/" + le.id;
        const String path = String(SHOTS_DIR) + "/" + le.id + ".slog";
        if (postFile(url, token, "application/octet-stream", fs, path)) {
            ESP_LOGI(LOG_TAG, "Uploaded slog %s (%lld bytes)", le.id.c_str(), (long long)le.slogSize);
            uploaded++;
        } else {
            ESP_LOGW(LOG_TAG, "Failed to upload slog %s", le.id.c_str());
        }
    }

    ESP_LOGI(LOG_TAG, "Sync round done: uploaded=%d", uploaded);
    return true;
}
