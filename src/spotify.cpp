#include "spotify.h"
#include "app_log.h"
#include "board.h"
#include "config.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

namespace {

const char *TOKEN_URL = "https://accounts.spotify.com/api/token";
const char *API_BASE = "https://api.spotify.com/v1";
constexpr size_t MAX_ART_BYTES = 400 * 1024;
constexpr uint32_t POLL_MS = 3000;
constexpr uint32_t COMMAND_REPOLL_MS = 800;
const char *LIST_TMP_PATH = "/list.json";

WiFiClientSecure sClient;
String sClientHost;

SemaphoreHandle_t netLock;
SemaphoreHandle_t stateLock;
QueueHandle_t cmdQueue;
bool taskStarted = false;

PlayerState shared;
SemaphoreHandle_t artLock;
volatile bool artReady = false;
uint32_t lastPollMs = 0;
volatile uint32_t lastOkMs = 0;
volatile bool everOk = false;

struct CmdMsg {
    uint8_t cmd;
    char arg[96];
};

ListData lists[LIST_KIND_COUNT];

String accessToken;
uint32_t tokenExpiresAt = 0;
volatile bool authLost = false;
uint32_t backoffUntil = 0;
uint8_t backoffStep = 0;
String lastArtUrl;
uint8_t artFailures = 0;

// A growable PSRAM buffer that HTTPClient writes into and ArduinoJson reads from.
class MemStream : public Stream {
public:
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *data, size_t size) override {
        if (len + size > cap) {
            size_t newCap = max<size_t>(cap ? cap * 2 : 32 * 1024, len + size);
            auto *grown = (uint8_t *)heap_caps_realloc(buf, newCap, MALLOC_CAP_SPIRAM);
            if (!grown) return 0;
            buf = grown;
            cap = newCap;
        }
        memcpy(buf + len, data, size);
        len += size;
        return size;
    }
    int available() override { return len - pos; }
    int read() override { return pos < len ? buf[pos++] : -1; }
    int peek() override { return pos < len ? buf[pos] : -1; }
    void flush() override {}
    void clear() { len = pos = 0; }
    void rewind() { pos = 0; }
    const uint8_t *data() const { return buf; }
    size_t size() const { return len; }

private:
    uint8_t *buf = nullptr;
    size_t cap = 0;
    size_t len = 0;
    size_t pos = 0;
};

#if BOARD_TEMP_IN_PSRAM
MemStream listTemp;
MemStream artTemp;
#endif

struct HttpResult {
    int code = -1;
    String body;
    uint32_t retryAfter = 0;
    int savedBytes = 0;
};

bool reached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }

String urlEncode(const String &s) {
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            out += buf;
        }
    }
    return out;
}

String hostOf(const String &url) {
    int start = url.indexOf("://") + 3;
    int end = url.indexOf('/', start);
    return end < 0 ? url.substring(start) : url.substring(start, end);
}

void useHost(const String &host) {
    if (host != sClientHost) {
        sClient.stop();
        sClientHost = host;
    }
}

String describeError(const String &body, int code) {
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
        if (doc["error"].is<JsonObject>()) {
            const char *msg = doc["error"]["message"] | "";
            if (msg[0]) return msg;
        } else {
            const char *desc = doc["error_description"] | "";
            if (desc[0]) return desc;
            const char *err = doc["error"] | "";
            if (err[0]) return err;
        }
    }
    return code > 0 ? "HTTP " + String(code) : "No connection to Spotify. Long-press the screen to forget this Wi-Fi.";
}

HttpResult httpOnce(const char *method, const String &url, const char *contentType, const String &body, const String &bearer, const char *savePath) {
    HttpResult r;
    useHost(hostOf(url));

    HTTPClient http;
    http.setReuse(true);
    http.setConnectTimeout(6000);
    http.setTimeout(6000);
    const char *keys[] = {"Retry-After"};
    http.collectHeaders(keys, 1);

    if (!http.begin(sClient, url)) return r;
    if (bearer.length()) http.addHeader("Authorization", "Bearer " + bearer);
    if (contentType) http.addHeader("Content-Type", contentType);
    if (body.isEmpty() && strcmp(method, "GET") != 0) http.addHeader("Content-Length", "0");

    uint32_t t0 = millis();
    r.code = http.sendRequest(method, (uint8_t *)body.c_str(), body.length());
    // Without this guard getString() blocks until the server closes a 204 response.
    if (r.code == 200 && savePath) {
#if BOARD_TEMP_IN_PSRAM
        listTemp.clear();
        r.savedBytes = http.writeToStream(&listTemp);
#else
        File file = LittleFS.open(savePath, "w");
        if (file) {
            r.savedBytes = http.writeToStream(&file);
            file.close();
        }
        if (r.savedBytes <= 0) LittleFS.remove(savePath);
#endif
    } else if (r.code > 0 && r.code != 204 && http.getSize() != 0) {
        r.body = http.getString();
    }
    if (r.code > 0 && http.hasHeader("Retry-After")) r.retryAfter = http.header("Retry-After").toInt();
    http.end();

    String path = url.substring(url.indexOf('/', 8));
    if (r.code < 0) {
        app_log(LL_WARN, "%s %s failed: %s (8-bit heap free=%u, block=%u)", method, path.c_str(), HTTPClient::errorToString(r.code).c_str(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    } else if (r.code >= 400) {
        app_log(LL_WARN, "%s %s -> %d: %.160s", method, path.c_str(), r.code, r.body.c_str());
    } else {
        app_log(LL_DEBUG, "%s %s -> %d (%u ms, %u B)", method, path.c_str(), r.code, (unsigned)(millis() - t0), (unsigned)(r.body.length() + r.savedBytes));
    }
    return r;
}

HttpResult httpDo(const char *method, const String &url, const char *contentType, const String &body, const String &bearer, const char *savePath = nullptr) {
    bool reused = hostOf(url) == sClientHost && sClient.connected();
    HttpResult r = httpOnce(method, url, contentType, body, bearer, savePath);
    // A kept-alive connection the server already closed shows up as a timeout.
    if (r.code < 0 && reused) {
        sClient.stop();
        r = httpOnce(method, url, contentType, body, bearer, savePath);
    }
    // TLS setup fails with "connection refused" when heap is momentarily short, so wait and retry.
    for (int attempt = 0; attempt < 2 && r.code == HTTPC_ERROR_CONNECTION_REFUSED; attempt++) {
        sClient.stop();
        vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 400 : 1200));
        r = httpOnce(method, url, contentType, body, bearer, savePath);
    }
    return r;
}

String volumeRefusedBy;
// The play queue, fetched on each track change and every QUEUE_REFRESH_MS.
constexpr uint32_t QUEUE_REFRESH_MS = 30000;
constexpr size_t QUEUE_MAX = 10;
String queueForId;
uint32_t queueFetchedAt = 0;
std::vector<String> queueItems;  // guarded by stateLock
String previousLine;             // guarded by stateLock
bool recentNeedsLogin = false;

// Liked Songs status of the current track, fetched once per track.
String likedForId;
int8_t likedValue = -1;
bool likeNeedsLogin = false;

void setMessage(const String &msg) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    shared.message = msg;
    xSemaphoreGive(stateLock);
}

void noteRateLimit(const HttpResult &r) {
    uint32_t wait = r.retryAfter;
    if (wait == 0) {
        wait = min<uint32_t>(60, 2u << backoffStep);
        if (backoffStep < 5) backoffStep++;
    }
    backoffUntil = millis() + wait * 1000;
    setMessage("Rate limited, retrying in " + String(wait) + "s");
}

bool backoffActive() { return !reached(backoffUntil); }

bool storeTokens(const String &body) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        app_log(LL_ERROR, "Token response is not valid JSON");
        return false;
    }
    const char *token = doc["access_token"] | "";
    if (!token[0]) {
        app_log(LL_ERROR, "Token response has no access_token");
        return false;
    }

    accessToken = token;
    uint32_t expires = doc["expires_in"] | 3600;
    tokenExpiresAt = millis() + (expires > 120 ? expires - 60 : expires) * 1000UL;

    const char *refresh = doc["refresh_token"] | "";
    if (refresh[0] && g_config.refreshToken != refresh) {
        g_config.refreshToken = refresh;
        config_save();
    }
    return true;
}

bool refreshAccessToken() {
    String body = "grant_type=refresh_token&refresh_token=" + urlEncode(g_config.refreshToken) +
                  "&client_id=" + urlEncode(g_config.clientId);
    HttpResult r = httpDo("POST", TOKEN_URL, "application/x-www-form-urlencoded", body, "");

    if (r.code == 200 && storeTokens(r.body)) {
        app_log(LL_INFO, "Access token refreshed");
        backoffStep = 0;
        return true;
    }
    if (r.code == 400 || r.code == 401) {
        app_log(LL_ERROR, "Refresh token rejected, Spotify login required again");
        g_config.refreshToken = "";
        config_save();
        accessToken = "";
        authLost = true;
    } else if (r.code == 429) {
        noteRateLimit(r);
    } else {
        setMessage(describeError(r.body, r.code));
    }
    return false;
}

HttpResult apiCall(const char *method, const String &path, const String &body = "", const char *savePath = nullptr) {
    HttpResult r;
    if (accessToken.isEmpty() || reached(tokenExpiresAt)) {
        if (!refreshAccessToken()) return r;
    }

    String url = String(API_BASE) + path;
    const char *contentType = body.length() ? "application/json" : nullptr;
    r = httpDo(method, url, contentType, body, accessToken, savePath);
    if (r.code == 401) {
        if (!refreshAccessToken()) return r;
        r = httpDo(method, url, contentType, body, accessToken, savePath);
    }
    if (r.code == 429) noteRateLimit(r);
    return r;
}

String pickImage(JsonArray images, int target, String *smallest = nullptr) {
    String url;
    int best = INT_MAX;
    int smallestWidth = INT_MAX;
    for (JsonObject img : images) {
        const char *u = img["url"] | "";
        if (!u[0]) continue;
        int width = img["width"] | 0;
        int diff = abs(width - target);
        if (diff < best) {
            best = diff;
            url = u;
        }
        if (smallest && width < smallestWidth) {
            smallestWidth = width;
            *smallest = u;
        }
    }
    return url;
}

bool parsePlayer(const String &body, PlayerState &out) {
    JsonDocument filter;
    filter["is_playing"] = true;
    filter["shuffle_state"] = true;
    filter["repeat_state"] = true;
    filter["progress_ms"] = true;
    filter["device"]["volume_percent"] = true;
    filter["device"]["supports_volume"] = true;
    filter["device"]["name"] = true;
    filter["device"]["type"] = true;
    JsonObject item = filter["item"].to<JsonObject>();
    item["id"] = true;
    item["name"] = true;
    item["type"] = true;
    item["duration_ms"] = true;
    item["explicit"] = true;
    item["track_number"] = true;
    item["disc_number"] = true;
    item["release_date"] = true;
    item["external_ids"]["isrc"] = true;
    item["artists"][0]["name"] = true;
    item["album"]["name"] = true;
    item["album"]["id"] = true;
    item["album"]["album_type"] = true;
    item["album"]["release_date"] = true;
    item["album"]["total_tracks"] = true;
    item["album"]["images"][0]["url"] = true;
    item["album"]["images"][0]["width"] = true;
    item["show"]["name"] = true;
    item["images"][0]["url"] = true;
    item["images"][0]["width"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        app_log(LL_ERROR, "Player JSON error: %s (%u B): %.150s", err.c_str(), (unsigned)body.length(), body.c_str());
        return false;
    }

    out.playing = doc["is_playing"] | false;
    out.shuffle = doc["shuffle_state"] | false;
    const char *repeat = doc["repeat_state"] | "off";
    out.repeat = strcmp(repeat, "track") == 0 ? REPEAT_TRACK : strcmp(repeat, "context") == 0 ? REPEAT_CONTEXT : REPEAT_OFF;
    out.progressMs = doc["progress_ms"] | 0;
    out.volume = doc["device"]["volume_percent"] | -1;
    out.volumeAllowed = doc["device"]["supports_volume"] | true;
    out.deviceName = (const char *)(doc["device"]["name"] | "");
    out.deviceType = (const char *)(doc["device"]["type"] | "");

    JsonObject it = doc["item"];
    if (it.isNull()) return true;

    out.active = true;
    out.trackId = (const char *)(it["id"] | "");
    out.title = (const char *)(it["name"] | "");
    out.durationMs = it["duration_ms"] | 0;
    out.isExplicit = it["explicit"] | false;
    out.trackNumber = it["track_number"] | 0;
    out.discNumber = it["disc_number"] | 0;
    out.isrc = (const char *)(it["external_ids"]["isrc"] | "");

    if (strcmp(it["type"] | "", "episode") == 0) {
        out.isEpisode = true;
        out.artist = (const char *)(it["show"]["name"] | "");
        out.releaseDate = (const char *)(it["release_date"] | "");
        out.artUrl = pickImage(it["images"], g_config.artSize, &out.artUrlSmall);
    } else {
        out.album = (const char *)(it["album"]["name"] | "");
        out.albumId = (const char *)(it["album"]["id"] | "");
        out.albumType = (const char *)(it["album"]["album_type"] | "");
        out.releaseDate = (const char *)(it["album"]["release_date"] | "");
        out.totalTracks = it["album"]["total_tracks"] | 0;
        for (JsonObject a : it["artists"].as<JsonArray>()) {
            if (out.artist.length()) out.artist += ", ";
            out.artist += (const char *)(a["name"] | "");
        }
        out.artUrl = pickImage(it["album"]["images"], g_config.artSize, &out.artUrlSmall);
    }
    return true;
}

bool downloadArt(const String &url) {
    // Drop the API TLS session first: two sessions do not fit in heap.
    sClient.stop();
    sClientHost = "";
    app_log(LL_DEBUG, "Art: %s (free=%u, max=%u)", url.c_str(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

    xSemaphoreTake(artLock, portMAX_DELAY);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    bool ok = false;
    int code = -1;
    bool begun = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) {
            sClient.stop();
            vTaskDelay(pdMS_TO_TICKS(attempt * 800));
        }
        begun = http.begin(sClient, url);
        if (!begun) break;
        code = http.GET();
        if (code != HTTPC_ERROR_CONNECTION_REFUSED) break;
        http.end();
    }
    if (begun) {
        int size = http.getSize();
        if (code != 200) {
            app_log(LL_WARN, "Art: HTTP %d", code);
        } else if (size > (int)MAX_ART_BYTES) {
            app_log(LL_WARN, "Art: image too large (%d bytes)", size);
        } else {
#if BOARD_TEMP_IN_PSRAM
            artTemp.clear();
            int written = http.writeToStream(&artTemp);
            ok = written > 0;
            if (!ok) app_log(LL_WARN, "Art: download failed (%d)", written);
#else
            File file = LittleFS.open(SPOTIFY_ART_PATH, "w");
            if (!file) {
                app_log(LL_WARN, "Art: cannot open temp file");
            } else {
                int written = http.writeToStream(&file);
                file.close();
                ok = written > 0;
                if (!ok) {
                    app_log(LL_WARN, "Art: download failed (%d)", written);
                    LittleFS.remove(SPOTIFY_ART_PATH);
                }
            }
#endif
        }
        http.end();
    } else {
        app_log(LL_WARN, "Art: begin failed");
    }
    sClient.stop();
    if (ok) artReady = true;
    xSemaphoreGive(artLock);
    return ok;
}

// Looks up whether the track is in Liked Songs (once per track; episodes are skipped). Uses the generic
// library endpoints: Spotify removed /me/tracks for Development Mode apps in February 2026.
void checkLiked(const PlayerState &s) {
    likedForId = s.trackId;
    likedValue = -1;
    if (s.isEpisode || s.trackId.isEmpty() || likeNeedsLogin) return;
    HttpResult r = apiCall("GET", "/me/library/contains?uris=spotify%3Atrack%3A" + s.trackId);
    if (r.code == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, r.body) && doc.is<JsonArray>()) likedValue = doc[0] ? 1 : 0;
    } else if (r.code == 403) {
        likeNeedsLogin = true;
    } else if (r.code == 429) {
        likedForId = "";  // try again on a later poll
        return;
    } else {
        app_log(LL_WARN, "Liked check failed: HTTP %d", r.code);
    }
    xSemaphoreTake(stateLock, portMAX_DELAY);
    if (shared.trackId == s.trackId) {
        shared.liked = likedValue;
        shared.likeNeedsLogin = likeNeedsLogin;
    }
    xSemaphoreGive(stateLock);
}

DeserializationError parseSaved(JsonDocument &doc, JsonDocument &filter);

// Reads the next tracks of the play queue. The response holds full track objects, so it is streamed to
// the temp buffer and filtered.
void fetchQueue(const PlayerState &s) {
    queueForId = s.trackId;
    queueFetchedAt = millis();
    HttpResult r = apiCall("GET", "/me/player/queue", "", LIST_TMP_PATH);
    if (r.code != 200 || r.savedBytes <= 0) {
        if (r.code != 429) app_log(LL_WARN, "Queue: HTTP %d", r.code);
        return;
    }
    JsonDocument filter;
    filter["queue"][0]["name"] = true;
    filter["queue"][0]["artists"][0]["name"] = true;
    filter["queue"][0]["show"]["name"] = true;
    JsonDocument doc;
    DeserializationError err = parseSaved(doc, filter);
    if (err) {
        app_log(LL_WARN, "Queue JSON error: %s", err.c_str());
        return;
    }
    std::vector<String> items;
    for (JsonObject t : doc["queue"].as<JsonArray>()) {
        if (items.size() >= QUEUE_MAX) break;
        String line = (const char *)(t["name"] | "");
        const char *by = t["artists"][0]["name"] | (const char *)(t["show"]["name"] | "");
        if (by[0]) line += String(" - ") + by;
        if (line.length()) items.push_back(line);
    }
    xSemaphoreTake(stateLock, portMAX_DELAY);
    queueItems = std::move(items);
    if (shared.trackId == s.trackId) shared.upNext = queueItems.empty() ? String() : queueItems[0];
    xSemaphoreGive(stateLock);
}

// The last track played before the current one, from the recently played list (a track only appears there
// once it has played for a while, so this catches up within a refresh).
void fetchPrevious(const PlayerState &s) {
    if (recentNeedsLogin) return;
    HttpResult r = apiCall("GET", "/me/player/recently-played?limit=3", "", LIST_TMP_PATH);
    if (r.code == 403) {
        recentNeedsLogin = true;
        return;
    }
    if (r.code != 200 || r.savedBytes <= 0) return;
    JsonDocument filter;
    filter["items"][0]["track"]["id"] = true;
    filter["items"][0]["track"]["name"] = true;
    filter["items"][0]["track"]["artists"][0]["name"] = true;
    JsonDocument doc;
    if (parseSaved(doc, filter)) return;
    String line;
    for (JsonObject o : doc["items"].as<JsonArray>()) {
        if (s.trackId == (const char *)(o["track"]["id"] | "")) continue;
        line = (const char *)(o["track"]["name"] | "");
        const char *artist = o["track"]["artists"][0]["name"] | "";
        if (artist[0]) line += String(" - ") + artist;
        break;
    }
    xSemaphoreTake(stateLock, portMAX_DELAY);
    previousLine = line;
    if (shared.trackId == s.trackId) shared.previous = line;
    xSemaphoreGive(stateLock);
}

void pollPlayer() {
    uint32_t t0 = millis();
    HttpResult r = apiCall("GET", "/me/player?additional_types=track,episode");
    lastPollMs = millis() - t0;
    if (r.code == 200 || r.code == 204) {
        lastOkMs = millis();
        everOk = true;
    }

    if (r.code == 200 && r.body.length()) {
        PlayerState s;
        if (!parsePlayer(r.body, s)) {
            setMessage("Unexpected response from Spotify");
            return;
        }
        s.stamp = millis();
        if (s.deviceName == volumeRefusedBy) s.volumeAllowed = false;
        s.likeNeedsLogin = likeNeedsLogin;
        if (s.trackId == likedForId) s.liked = likedValue;
        if (s.trackId == queueForId) {
            xSemaphoreTake(stateLock, portMAX_DELAY);
            if (!queueItems.empty()) s.upNext = queueItems[0];
            s.previous = previousLine;
            xSemaphoreGive(stateLock);
        }
        xSemaphoreTake(stateLock, portMAX_DELAY);
        shared = s;
        xSemaphoreGive(stateLock);
        if (s.trackId != likedForId) checkLiked(s);
        if (s.active && (s.trackId != queueForId || millis() - queueFetchedAt > QUEUE_REFRESH_MS)) {
            fetchQueue(s);
            fetchPrevious(s);
        }
        backoffStep = 0;
        app_log(LL_DEBUG, "Now: %s - %s (%s)", s.artist.c_str(), s.title.c_str(), s.playing ? "playing" : "paused");

        if (s.artUrl.length() && s.artUrl != lastArtUrl) {
            r.body = String();
            bool got = downloadArt(s.artUrl);
            if (!got && s.artUrlSmall.length() && s.artUrlSmall != s.artUrl) got = downloadArt(s.artUrlSmall);
            if (got || ++artFailures >= 3) {
                lastArtUrl = s.artUrl;
                artFailures = 0;
            }
        }
    } else if (r.code == 204 || r.code == 200) {
        xSemaphoreTake(stateLock, portMAX_DELAY);
        shared = PlayerState();
        shared.message = "No active device. Start playback in Spotify.";
        xSemaphoreGive(stateLock);
    } else if (r.code != 429 && !authLost) {
        setMessage(describeError(r.body, r.code));
    }
}

void finishList(ListKind kind, ListStatus status, const String &error, std::vector<ListItem> &&items,
                uint32_t offset = 0, uint32_t next = 0, uint32_t total = 0) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    ListData &l = lists[kind];
    l.status = status;
    l.error = error;
    l.items = std::move(items);
    l.offset = offset;
    l.next = next;
    l.total = total;
    l.version++;
    xSemaphoreGive(stateLock);
}

String listError(const HttpResult &r) {
    if (r.code == 403) return "Permission missing. Login to Spotify again on the web page.";
    return describeError(r.body, r.code);
}

int listKindOfCmd(uint8_t cmd) {
    switch (cmd) {
    case CMD_LIST_DEVICES:
        return LIST_DEVICES;
    case CMD_LIST_PLAYLISTS:
        return LIST_PLAYLISTS;
    case CMD_LIST_WEB_PLAYLISTS:
        return LIST_WEB_PLAYLISTS;
    case CMD_LOOKUP_PLAYLIST:
        return LIST_WEB_LOOKUP;
    case CMD_SEARCH_PLAYLISTS:
        return LIST_WEB_SEARCH;
    case CMD_LIST_RECENT:
        return LIST_RECENT;
    default:
        return -1;
    }
}

// Parses a response that apiCall saved with LIST_TMP_PATH, then discards it.
DeserializationError parseSaved(JsonDocument &doc, JsonDocument &filter) {
#if BOARD_TEMP_IN_PSRAM
    listTemp.rewind();
    DeserializationError err = deserializeJson(doc, listTemp, DeserializationOption::Filter(filter));
    listTemp.clear();
#else
    File file = LittleFS.open(LIST_TMP_PATH, "r");
    DeserializationError err = deserializeJson(doc, file, DeserializationOption::Filter(filter));
    file.close();
    LittleFS.remove(LIST_TMP_PATH);
#endif
    return err;
}

void fetchList(ListKind kind, uint32_t offset) {
    bool playlists = kind != LIST_DEVICES;
    // Playlist pages are too large for one RAM buffer, so they are streamed to a temp file.
    HttpResult r = playlists ? apiCall("GET", "/me/playlists?limit=50&offset=" + String(offset), "", LIST_TMP_PATH)
                             : apiCall("GET", "/me/player/devices");
    if (r.code != 200 || (playlists && r.savedBytes <= 0)) {
        finishList(kind, LIST_ERROR, listError(r), {}, offset);
        return;
    }

    JsonDocument filter;
    if (playlists) {
        filter["total"] = true;
        filter["items"][0]["id"] = true;
        filter["items"][0]["name"] = true;
        filter["items"][0]["owner"]["display_name"] = true;
    } else {
        filter["devices"][0]["id"] = true;
        filter["devices"][0]["name"] = true;
        filter["devices"][0]["is_active"] = true;
    }
    JsonDocument doc;
    DeserializationError err;
    if (playlists) {
        err = parseSaved(doc, filter);
    } else {
        err = deserializeJson(doc, r.body, DeserializationOption::Filter(filter));
    }
    if (err) {
        app_log(LL_ERROR, "List JSON error: %s", err.c_str());
        finishList(kind, LIST_ERROR, "Unexpected response from Spotify", {}, offset);
        return;
    }

    JsonArray array = doc[playlists ? "items" : "devices"].as<JsonArray>();
    std::vector<ListItem> items;
    for (JsonObject o : array) {
        const char *id = o["id"] | "";
        if (!id[0]) continue;
        ListItem item;
        item.id = id;
        item.name = (const char *)(o["name"] | "");
        item.active = o["is_active"] | false;
        item.owner = (const char *)(o["owner"]["display_name"] | "");
        items.push_back(std::move(item));
    }
    finishList(kind, LIST_READY, "", std::move(items), offset, offset + array.size(), doc["total"] | 0);
}

// Recently played tracks, newest first, without repeats. Full track objects, so streamed and filtered.
void fetchRecent() {
    HttpResult r = apiCall("GET", "/me/player/recently-played?limit=40", "", LIST_TMP_PATH);
    if (r.code != 200 || r.savedBytes <= 0) {
        finishList(LIST_RECENT, LIST_ERROR, listError(r), {});
        return;
    }
    JsonDocument filter;
    filter["items"][0]["track"]["id"] = true;
    filter["items"][0]["track"]["name"] = true;
    filter["items"][0]["track"]["artists"][0]["name"] = true;
    filter["items"][0]["context"]["uri"] = true;
    JsonDocument doc;
    if (DeserializationError err = parseSaved(doc, filter)) {
        app_log(LL_ERROR, "Recent JSON error: %s", err.c_str());
        finishList(LIST_RECENT, LIST_ERROR, "Unexpected response from Spotify", {});
        return;
    }
    std::vector<ListItem> items;
    for (JsonObject o : doc["items"].as<JsonArray>()) {
        const char *id = o["track"]["id"] | "";
        if (!id[0]) continue;
        bool seen = false;
        for (const ListItem &item : items) seen = seen || item.id == id;
        if (seen) continue;
        ListItem item;
        item.id = id;
        item.name = (const char *)(o["track"]["name"] | "");
        const char *artist = o["track"]["artists"][0]["name"] | "";
        if (artist[0]) item.name += String(" - ") + artist;
        item.context = (const char *)(o["context"]["uri"] | "");
        items.push_back(std::move(item));
        if (items.size() >= 20) break;
    }
    uint32_t count = items.size();
    finishList(LIST_RECENT, LIST_READY, "", std::move(items), 0, 0, count);
}

// arg is "trackId|contextUri|positionMs". Artist contexts cannot start at a given track, so those play the track alone,
// as does any context Spotify refuses.
HttpResult playTrack(const char *arg) {
    String a = arg;
    int bar = a.indexOf('|');
    int bar2 = bar < 0 ? -1 : a.indexOf('|', bar + 1);
    String trackUri = "spotify:track:" + (bar < 0 ? a : a.substring(0, bar));
    String context = bar < 0 ? String() : a.substring(bar + 1, bar2 < 0 ? a.length() : bar2);
    uint32_t position = bar2 < 0 ? 0 : strtoul(a.c_str() + bar2 + 1, nullptr, 10);
    if (context.startsWith("spotify:artist:")) context = "";
    JsonDocument body;
    String json;
    if (context.length()) {
        body["context_uri"] = context;
        body["offset"]["uri"] = trackUri;
        if (position) body["position_ms"] = position;
        serializeJson(body, json);
        HttpResult r = apiCall("PUT", "/me/player/play", json);
        if (r.code < 400 || r.code == 401 || r.code == 429) return r;
        app_log(LL_WARN, "Play in %s failed (HTTP %d), playing the track alone", context.c_str(), r.code);
        body.clear();
        json = "";
    }
    body["uris"][0] = trackUri;
    if (position) body["position_ms"] = position;
    serializeJson(body, json);
    return apiCall("PUT", "/me/player/play", json);
}

void fetchLookup(const String &id) {
    HttpResult r = apiCall("GET", "/playlists/" + id + "?fields=id,name,owner(display_name)");
    if (r.code != 200) {
        finishList(LIST_WEB_LOOKUP, LIST_ERROR, r.code == 404 ? String("Not available through the Spotify API") : listError(r), {});
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, r.body)) {
        finishList(LIST_WEB_LOOKUP, LIST_ERROR, "Unexpected response from Spotify", {});
        return;
    }
    std::vector<ListItem> items;
    ListItem item;
    item.id = (const char *)(doc["id"] | "");
    item.name = (const char *)(doc["name"] | "");
    item.owner = (const char *)(doc["owner"]["display_name"] | "");
    items.push_back(std::move(item));
    finishList(LIST_WEB_LOOKUP, LIST_READY, "", std::move(items), 0, 0, 1);
}

void fetchSearch(uint32_t offset, const String &query) {
    String path = "/search?type=playlist&limit=10&offset=" + String(offset) + "&q=" + urlEncode(query);
    HttpResult r = apiCall("GET", path, "", LIST_TMP_PATH);
    if (r.code != 200 || r.savedBytes <= 0) {
        finishList(LIST_WEB_SEARCH, LIST_ERROR, listError(r), {}, offset);
        return;
    }

    JsonDocument filter;
    filter["playlists"]["total"] = true;
    filter["playlists"]["items"][0]["id"] = true;
    filter["playlists"]["items"][0]["name"] = true;
    filter["playlists"]["items"][0]["owner"]["display_name"] = true;
    JsonDocument doc;
    DeserializationError err = parseSaved(doc, filter);
    if (err) {
        app_log(LL_ERROR, "Search JSON error: %s", err.c_str());
        finishList(LIST_WEB_SEARCH, LIST_ERROR, "Unexpected response from Spotify", {}, offset);
        return;
    }

    JsonArray array = doc["playlists"]["items"].as<JsonArray>();
    std::vector<ListItem> items;
    for (JsonObject o : array) {
        const char *id = o["id"] | "";
        if (!id[0]) continue;
        ListItem item;
        item.id = id;
        item.name = (const char *)(o["name"] | "");
        item.owner = (const char *)(o["owner"]["display_name"] | "");
        items.push_back(std::move(item));
    }
    finishList(LIST_WEB_SEARCH, LIST_READY, "", std::move(items), offset, offset + array.size(), doc["playlists"]["total"] | 0);
}

void runCommand(const CmdMsg &msg) {
    PlayerState snap;
    spotify_get_state(snap);

    HttpResult r;
    int newVolume = snap.volume;
    switch (msg.cmd) {
    case CMD_PLAY_PAUSE:
        r = apiCall("PUT", snap.playing ? "/me/player/pause" : "/me/player/play");
        break;
    case CMD_NEXT:
        r = apiCall("POST", "/me/player/next");
        break;
    case CMD_PREV:
        r = apiCall("POST", "/me/player/previous");
        break;
    case CMD_VOL_UP:
    case CMD_VOL_DOWN:
        if (snap.volume < 0) return;
        newVolume = constrain(snap.volume + (msg.cmd == CMD_VOL_UP ? 10 : -10), 0, 100);
        r = apiCall("PUT", "/me/player/volume?volume_percent=" + String(newVolume));
        if (r.code == 403) {
            // Some devices claim volume support but still refuse it; remember that until the device changes.
            volumeRefusedBy = snap.deviceName;
            xSemaphoreTake(stateLock, portMAX_DELAY);
            shared.volumeAllowed = false;
            shared.message = VOLUME_NOT_ALLOWED;
            xSemaphoreGive(stateLock);
            return;
        }
        break;
    case CMD_LIST_DEVICES:
        fetchList(LIST_DEVICES, 0);
        return;
    case CMD_LIST_PLAYLISTS:
        fetchList(LIST_PLAYLISTS, strtoul(msg.arg, nullptr, 10));
        return;
    case CMD_LIST_RECENT:
        fetchRecent();
        return;
    case CMD_PLAY_TRACK:
        r = playTrack(msg.arg);
        break;
    case CMD_LIST_WEB_PLAYLISTS:
        fetchList(LIST_WEB_PLAYLISTS, strtoul(msg.arg, nullptr, 10));
        return;
    case CMD_LOOKUP_PLAYLIST: {
        const char *colon = strchr(msg.arg, ':');
        fetchLookup(colon ? colon + 1 : "");
        return;
    }
    case CMD_SEARCH_PLAYLISTS: {
        char *colon = nullptr;
        uint32_t offset = strtoul(msg.arg, &colon, 10);
        fetchSearch(offset, colon && *colon == ':' ? String(colon + 1) : String());
        return;
    }
    case CMD_TRANSFER: {
        JsonDocument body;
        body["device_ids"][0] = (const char *)msg.arg;
        body["play"] = true;
        String json;
        serializeJson(body, json);
        r = apiCall("PUT", "/me/player", json);
        break;
    }
    case CMD_SEEK:
        r = apiCall("PUT", "/me/player/seek?position_ms=" + String(strtoul(msg.arg, nullptr, 10)));
        break;
    case CMD_SHUFFLE:
        r = apiCall("PUT", String("/me/player/shuffle?state=") + (snap.shuffle ? "false" : "true"));
        break;
    case CMD_REPEAT: {
        static const char *const MODES[] = {"off", "context", "track"};
        r = apiCall("PUT", String("/me/player/repeat?state=") + MODES[(snap.repeat + 1) % 3]);
        break;
    }
    case CMD_LIKE:
        if (snap.isEpisode || snap.trackId.isEmpty() || snap.liked < 0) return;
        r = apiCall(snap.liked ? "DELETE" : "PUT", "/me/library?uris=spotify%3Atrack%3A" + snap.trackId);
        if (r.code == 403) {
            likeNeedsLogin = true;
            xSemaphoreTake(stateLock, portMAX_DELAY);
            shared.likeNeedsLogin = true;
            shared.liked = -1;
            xSemaphoreGive(stateLock);
            setMessage(LIKE_NEEDS_LOGIN);
            return;
        }
        break;
    case CMD_PLAY_CONTEXT: {
        JsonDocument body;
        body["context_uri"] = String("spotify:playlist:") + msg.arg;
        String json;
        serializeJson(body, json);
        r = apiCall("PUT", "/me/player/play", json);
        break;
    }
    default:
        return;
    }

    if (r.code >= 200 && r.code < 300) {
        xSemaphoreTake(stateLock, portMAX_DELAY);
        if (msg.cmd == CMD_PLAY_PAUSE) shared.playing = !snap.playing;
        if (msg.cmd == CMD_SHUFFLE) shared.shuffle = !snap.shuffle;
        if (msg.cmd == CMD_REPEAT) shared.repeat = (RepeatMode)((snap.repeat + 1) % 3);
        if (msg.cmd == CMD_LIKE && shared.trackId == snap.trackId) {
            shared.liked = snap.liked ? 0 : 1;
            likedValue = shared.liked;
        }
        if (msg.cmd == CMD_VOL_UP || msg.cmd == CMD_VOL_DOWN) shared.volume = newVolume;
        if (msg.cmd == CMD_SEEK) {
            shared.progressMs = strtoul(msg.arg, nullptr, 10);
            shared.stamp = millis();
        }
        shared.message = "";
        xSemaphoreGive(stateLock);
    } else if (r.code != 429 && !authLost) {
        setMessage(describeError(r.body, r.code));
    }
}

void spotifyTask(void *) {
    uint32_t nextPoll = millis();
    for (;;) {
        if (authLost) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int32_t wait = (int32_t)(nextPoll - millis());
        CmdMsg msg = {};
        bool gotCmd = xQueueReceive(cmdQueue, &msg, pdMS_TO_TICKS(wait > 0 ? wait : 0)) == pdTRUE;

        xSemaphoreTake(netLock, portMAX_DELAY);
        bool online = WiFi.status() == WL_CONNECTED;
        if (gotCmd && online && !backoffActive()) {
            runCommand(msg);
            nextPoll = millis() + COMMAND_REPOLL_MS;
        } else if (gotCmd && listKindOfCmd(msg.cmd) >= 0) {
            finishList((ListKind)listKindOfCmd(msg.cmd), LIST_ERROR, online ? "Rate limited, try again shortly" : "Wi-Fi disconnected", {}, strtoul(msg.arg, nullptr, 10));
        }
        if (reached(nextPoll)) {
            if (!online) {
                setMessage("Wi-Fi disconnected");
            } else if (!backoffActive()) {
                pollPlayer();
            }
            nextPoll = millis() + POLL_MS;
            if (backoffActive() && (int32_t)(backoffUntil - nextPoll) > 0) nextPoll = backoffUntil;
        }
        xSemaphoreGive(netLock);
    }
}

} // namespace

void spotify_init() {
    netLock = xSemaphoreCreateMutex();
    stateLock = xSemaphoreCreateMutex();
    artLock = xSemaphoreCreateMutex();
    cmdQueue = xQueueCreate(4, sizeof(CmdMsg));
    sClient.setInsecure();
}

void spotify_start() {
    if (taskStarted) return;
    taskStarted = true;
    xTaskCreatePinnedToCore(spotifyTask, "spotify", 12288, nullptr, 1, nullptr, 0);
}

bool spotify_exchange_code(const String &code, const String &verifier, String &error) {
    xSemaphoreTake(netLock, portMAX_DELAY);
    String body = "grant_type=authorization_code&code=" + urlEncode(code) +
                  "&redirect_uri=" + urlEncode(SPOTIFY_REDIRECT_URI) +
                  "&client_id=" + urlEncode(g_config.clientId) +
                  "&code_verifier=" + urlEncode(verifier);
    HttpResult r = httpDo("POST", TOKEN_URL, "application/x-www-form-urlencoded", body, "");

    bool ok = r.code == 200 && storeTokens(r.body);
    if (ok) {
        authLost = false;
        likeNeedsLogin = false;
        likedForId = "";
        recentNeedsLogin = false;
        queueForId = "";
        backoffStep = 0;
        backoffUntil = millis();
        lastArtUrl = "";
    } else {
        error = describeError(r.body, r.code);
    }
    xSemaphoreGive(netLock);
    return ok;
}

void spotify_send(SpotifyCmd cmd) {
    CmdMsg msg = {};
    msg.cmd = cmd;
    xQueueSend(cmdQueue, &msg, 0);
}

void spotify_seek(uint32_t positionMs) {
    CmdMsg msg = {};
    msg.cmd = CMD_SEEK;
    snprintf(msg.arg, sizeof(msg.arg), "%u", (unsigned)positionMs);
    xQueueSend(cmdQueue, &msg, 0);
}

void spotify_request_list(ListKind kind, uint32_t offset, const String &query) {
    String arg = String(offset) + ":" + query;
    xSemaphoreTake(stateLock, portMAX_DELAY);
    lists[kind].status = LIST_LOADING;
    lists[kind].items.clear();
    lists[kind].offset = offset;
    lists[kind].key = arg;
    lists[kind].version++;
    xSemaphoreGive(stateLock);

    CmdMsg msg = {};
    switch (kind) {
    case LIST_DEVICES:
        msg.cmd = CMD_LIST_DEVICES;
        break;
    case LIST_PLAYLISTS:
        msg.cmd = CMD_LIST_PLAYLISTS;
        break;
    case LIST_WEB_PLAYLISTS:
        msg.cmd = CMD_LIST_WEB_PLAYLISTS;
        break;
    case LIST_WEB_LOOKUP:
        msg.cmd = CMD_LOOKUP_PLAYLIST;
        break;
    case LIST_WEB_SEARCH:
        msg.cmd = CMD_SEARCH_PLAYLISTS;
        break;
    case LIST_RECENT:
        msg.cmd = CMD_LIST_RECENT;
        break;
    default:
        return;
    }
    strlcpy(msg.arg, arg.c_str(), sizeof(msg.arg));
    xQueueSend(cmdQueue, &msg, 0);
}

void spotify_select(ListKind kind, const String &id) {
    CmdMsg msg = {};
    msg.cmd = kind == LIST_DEVICES ? CMD_TRANSFER : CMD_PLAY_CONTEXT;
    strlcpy(msg.arg, id.c_str(), sizeof(msg.arg));
    xQueueSend(cmdQueue, &msg, 0);
}

void spotify_play_track(const String &trackId, const String &contextUri, uint32_t positionMs) {
    CmdMsg msg = {};
    msg.cmd = CMD_PLAY_TRACK;
    String arg = trackId + "|" + contextUri + "|" + String(positionMs);
    if (arg.length() >= sizeof(msg.arg)) arg = trackId;  // too long to carry the context
    strlcpy(msg.arg, arg.c_str(), sizeof(msg.arg));
    xQueueSend(cmdQueue, &msg, 0);
}

void spotify_get_list(ListKind kind, ListData &out) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    out = lists[kind];
    xSemaphoreGive(stateLock);
}

uint32_t spotify_last_poll_ms() { return lastPollMs; }

bool spotify_recently_ok(uint32_t withinMs) { return everOk && millis() - lastOkMs < withinMs; }

bool spotify_token_valid() { return !accessToken.isEmpty() && !reached(tokenExpiresAt); }

void spotify_get_queue(std::vector<String> &out) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    out = queueItems;
    xSemaphoreGive(stateLock);
}

void spotify_get_state(PlayerState &out) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    out = shared;
    xSemaphoreGive(stateLock);
}

bool spotify_art_begin_read() {
    if (!artReady || xSemaphoreTake(artLock, 0) != pdTRUE) return false;
    if (!artReady) {
        xSemaphoreGive(artLock);
        return false;
    }
    return true;
}

const uint8_t *spotify_art_data(size_t &size) {
#if BOARD_TEMP_IN_PSRAM
    size = artTemp.size();
    return artTemp.data();
#else
    size = 0;
    return nullptr;
#endif
}

void spotify_art_end_read() {
    artReady = false;
#if BOARD_TEMP_IN_PSRAM
    artTemp.clear();
#else
    LittleFS.remove(SPOTIFY_ART_PATH);
#endif
    xSemaphoreGive(artLock);
}

bool spotify_auth_lost() { return authLost; }

uint32_t spotify_list_version(ListKind kind) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    uint32_t v = lists[kind].version;
    xSemaphoreGive(stateLock);
    return v;
}
