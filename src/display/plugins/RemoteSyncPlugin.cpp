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
constexpr time_t FILE_QUIESCENCE_SECS = 10;
constexpr int HTTP_TIMEOUT_MS = 10000;
constexpr const char *SHOTS_DIR = "/h";

struct ManifestEntry {
    String id;
    int64_t slogSize = 0;
    bool hasNotes = false;
    int64_t notesMtime = 0;
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

// Walks /h/ and returns one entry per shot whose .slog and .json files have
// both been quiescent (no writes) for FILE_QUIESCENCE_SECS. The quiescence
// window protects against uploading a partial file while ShotHistoryPlugin's
// extended-recording window is still open.
std::vector<ManifestEntry> readLocalManifest(FS *fs) {
    std::vector<ManifestEntry> out;
    File dir = fs->open(SHOTS_DIR);
    if (!dir || !dir.isDirectory()) {
        return out;
    }

    struct Pair {
        String id;
        int64_t slogSize = -1; // -1 means no .slog seen yet
        bool hasNotes = false;
        int64_t notesMtime = 0;
        bool quiescent = true;
    };
    std::vector<Pair> pairs;
    auto findOrInsert = [&](const String &id) -> Pair & {
        for (auto &p : pairs) {
            if (p.id == id)
                return p;
        }
        pairs.push_back({id, -1, false, 0, true});
        return pairs.back();
    };

    const time_t now = ::time(nullptr);
    File f = dir.openNextFile();
    while (f) {
        String name = f.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0)
            name = name.substring(slash + 1);

        time_t mtime = f.getLastWrite();
        bool fresh = (mtime > 0) && (now > mtime) && ((now - mtime) < FILE_QUIESCENCE_SECS);

        if (name.endsWith(".slog")) {
            String id = name.substring(0, name.length() - 5);
            Pair &p = findOrInsert(id);
            p.slogSize = f.size();
            if (fresh)
                p.quiescent = false;
        } else if (name.endsWith(".json")) {
            String id = name.substring(0, name.length() - 5);
            Pair &p = findOrInsert(id);
            p.hasNotes = true;
            p.notesMtime = mtime > 0 ? static_cast<int64_t>(mtime) : 0;
            if (fresh)
                p.quiescent = false;
        }
        f = dir.openNextFile();
    }
    dir.close();

    for (auto &p : pairs) {
        if (p.slogSize < 0)
            continue; // orphan .json with no .slog
        if (!p.quiescent)
            continue;
        out.push_back({p.id, p.slogSize, p.hasNotes, p.notesMtime});
    }
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
        e.hasNotes = obj["has_notes"].as<bool>();
        e.notesMtime = obj["notes_mtime"].as<int64_t>();
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

bool deleteShot(const String &baseUrl, const String &token, const String &id) {
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(baseUrl + "/api/shots/" + id))
        return false;
    addAuth(http, token);
    int code = http.sendRequest("DELETE");
    http.end();
    // Treat 404 as success — the row's already gone, which is what we wanted.
    return (code >= 200 && code < 300) || code == HTTP_CODE_NOT_FOUND;
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
    pluginManager->on("controller:brew:end", [this](Event const &) { requestSync(); });

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
    int deleted = 0;

    // Local → server: upload missing or out-of-date entries.
    for (auto &le : local) {
        const ManifestEntry *se = findById(server, le.id);
        const bool needSlog = !se || se->slogSize != le.slogSize;
        const bool needNotes = le.hasNotes && (!se || !se->hasNotes || se->notesMtime != le.notesMtime);

        if (needSlog) {
            const String url = baseUrl + "/api/shots/" + le.id;
            const String path = String(SHOTS_DIR) + "/" + le.id + ".slog";
            if (postFile(url, token, "application/octet-stream", fs, path)) {
                ESP_LOGI(LOG_TAG, "Uploaded slog %s (%lld bytes)", le.id.c_str(), (long long)le.slogSize);
                uploaded++;
            } else {
                ESP_LOGW(LOG_TAG, "Failed to upload slog %s", le.id.c_str());
            }
        }
        if (needNotes) {
            const String url = baseUrl + "/api/shots/" + le.id + "/notes";
            const String path = String(SHOTS_DIR) + "/" + le.id + ".json";
            if (postFile(url, token, "application/json", fs, path)) {
                ESP_LOGI(LOG_TAG, "Uploaded notes %s", le.id.c_str());
                uploaded++;
            } else {
                ESP_LOGW(LOG_TAG, "Failed to upload notes %s", le.id.c_str());
            }
        }
    }

    // Server → device: anything on the server but not local → soft-delete.
    for (auto &se : server) {
        if (findById(local, se.id) == nullptr) {
            if (deleteShot(baseUrl, token, se.id)) {
                ESP_LOGI(LOG_TAG, "Soft-deleted ghost %s", se.id.c_str());
                deleted++;
            }
        }
    }

    ESP_LOGI(LOG_TAG, "Sync round done: uploaded=%d deleted=%d", uploaded, deleted);
    return true;
}
