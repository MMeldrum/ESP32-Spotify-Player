#pragma once
#include <Arduino.h>
#include <vector>

#define SPOTIFY_REDIRECT_URI "http://127.0.0.1:8080/callback"
#define SPOTIFY_ART_PATH "/art.jpg"
#define SPOTIFY_SCOPES "user-read-playback-state user-modify-playback-state playlist-read-private user-library-read user-library-modify"

enum SpotifyCmd : uint8_t {
    CMD_PLAY_PAUSE = 1,
    CMD_NEXT,
    CMD_PREV,
    CMD_VOL_UP,
    CMD_VOL_DOWN,
    CMD_LIST_DEVICES,
    CMD_LIST_PLAYLISTS,
    CMD_LIST_WEB_PLAYLISTS,
    CMD_LOOKUP_PLAYLIST,
    CMD_SEARCH_PLAYLISTS,
    CMD_SEEK,
    CMD_TRANSFER,
    CMD_PLAY_CONTEXT,
    CMD_SHUFFLE,
    CMD_REPEAT,
    CMD_LIKE,
};

enum RepeatMode : uint8_t { REPEAT_OFF, REPEAT_CONTEXT, REPEAT_TRACK };

enum ListKind : uint8_t { LIST_DEVICES, LIST_PLAYLISTS, LIST_WEB_PLAYLISTS, LIST_WEB_LOOKUP, LIST_WEB_SEARCH };
enum ListStatus : uint8_t { LIST_IDLE, LIST_LOADING, LIST_READY, LIST_ERROR };

struct ListItem {
    String id;
    String name;
    bool active = false;
    bool pinned = false;
    String owner;
};

struct ListData {
    ListStatus status = LIST_IDLE;
    std::vector<ListItem> items;
    String error;
    String key;
    uint32_t version = 0;
    uint32_t offset = 0;
    uint32_t next = 0;
    uint32_t total = 0;
};

struct PlayerState {
    bool active = false;
    bool playing = false;
    bool isExplicit = false;
    String trackId;
    String title;
    String artist;
    String album;
    String albumType;
    String releaseDate;
    String isrc;
    String deviceName;
    String deviceType;
    String artUrl;
    String artUrlSmall;
    String message;
    uint16_t trackNumber = 0;
    uint16_t discNumber = 0;
    uint16_t totalTracks = 0;
    uint32_t progressMs = 0;
    uint32_t durationMs = 0;
    int volume = -1;
    // False for devices (most phones, some speakers) that refuse remote volume changes.
    bool volumeAllowed = true;
    bool isEpisode = false;
    bool shuffle = false;
    RepeatMode repeat = REPEAT_OFF;
    // 1 when the track is in Liked Songs, 0 when not, -1 when unknown (episode, not checked yet, no access).
    int8_t liked = -1;
    // The login predates the Liked Songs permission: Like needs a new Spotify login from the web page.
    bool likeNeedsLogin = false;
    // First track in the play queue ("Title - Artist"), empty when unknown or the queue is empty.
    String upNext;
    uint32_t stamp = 0;
};

constexpr const char *VOLUME_NOT_ALLOWED = "This device doesn't allow volume control";
constexpr const char *LIKE_NEEDS_LOGIN = "To use Like, log in to Spotify again on the web page";

void spotify_init();
void spotify_start();
bool spotify_exchange_code(const String &code, const String &verifier, String &error);
void spotify_send(SpotifyCmd cmd);
void spotify_seek(uint32_t positionMs);
void spotify_request_list(ListKind kind, uint32_t offset = 0, const String &query = "");
void spotify_select(ListKind kind, const String &id);
void spotify_get_list(ListKind kind, ListData &out);
void spotify_get_state(PlayerState &out);
// The next tracks in the play queue (up to 10), as "Title - Artist".
void spotify_get_queue(std::vector<String> &out);
bool spotify_art_begin_read();
// The downloaded art between begin_read and end_read, or nullptr when it is in the SPOTIFY_ART_PATH file.
const uint8_t *spotify_art_data(size_t &size);
void spotify_art_end_read();
bool spotify_auth_lost();
uint32_t spotify_last_poll_ms();
bool spotify_recently_ok(uint32_t withinMs);
uint32_t spotify_list_version(ListKind kind);
bool spotify_token_valid();
