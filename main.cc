/**
 * @file    main.cc
 * @authors Stavros Avramidis
 */


/* C++ libs */
#include <atomic>
#include <cctype>
#include <chrono>
#include <locale>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <stack>
/* C libs */
#include <cstdio>
/* Qt */
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QTimer>

/* local libs*/
#include "discord_game_sdk.h"
#include "httplib.hh"
#include "json.hh"

#ifdef WIN32

#include "windows_api_hook.hh"

#elif defined(__APPLE__) or defined(__MACH__)

#include "osx_api_hook.hh"

#else
#error "Not supported target"
#endif

#define CURRENT_TIME std::time(nullptr)
#define HIFI_ASSET "hifi"

static long long APPLICATION_ID = 584458858731405315;
std::atomic<bool> isPresenceActive;
static char *countryCode = nullptr;

static std::string currentStatus;
static std::mutex currentSongMutex;


struct Song {
    enum AudioQualityEnum { master, hifi, normal };
    std::string title;
    std::string artist;
    std::string album;
    std::string url;
    char id[10];
    int64_t starttime;
    int64_t runtime;
    uint64_t pausedtime;
    uint_fast8_t trackNumber;
    uint_fast8_t volumeNumber;
    bool isPaused = false;
    AudioQualityEnum quality;
    bool loaded = false;


    void setQuality(const std::string &q) {
        if (q == "HI_RES") {
            quality = master;
        } else {
            quality = hifi;
        }
    }


    inline bool isHighRes() const noexcept {
        return quality == master;
    }


    friend std::ostream &operator<<(std::ostream &out, const Song &song) {
        out << song.title << " of " << song.album << " from " << song.artist << "(" << song.runtime << ")";
        return out;
    }
};



std::string urlEncode(const std::string &value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (std::string::value_type c: value) {
        if (isalnum((unsigned char) c) || c == '-' || c == '_' || c == '.' || c == '~')
            escaped << c;
        else {
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) << int((unsigned char) c);
            escaped << std::nouppercase;
        }
    }
    return escaped.str();
}


struct Application {
    struct IDiscordCore *core;
    struct IDiscordUsers *users;
};

struct Application app;


static void updateDiscordPresence(const Song &song) {
    struct IDiscordActivityManager *manager = app.core->get_activity_manager(app.core);

    if (isPresenceActive && song.loaded) {
        struct DiscordActivityTimestamps timestamps{};
        memset(&timestamps, 0, sizeof(timestamps));
        if (song.runtime) {
            timestamps.end = song.starttime + song.runtime + song.pausedtime;
        }
        timestamps.start = song.starttime;

        struct DiscordActivity activity{DiscordActivityType_Listening};
        memset(&activity, 0, sizeof(activity));
        activity.type = DiscordActivityType_Listening;
        activity.application_id = APPLICATION_ID;
        snprintf(activity.details, 128, "%s", song.title.c_str());
        snprintf(activity.state, 128, "%s", (song.artist + " - " + song.album).c_str());

        struct DiscordActivityAssets assets{};
        memset(&assets, 0, sizeof(assets));
        if (song.isPaused) {
            snprintf(assets.small_image, 128, "%s", "pause");
            snprintf(assets.small_text, 128, "%s", "Paused");
        } else {
            activity.timestamps = timestamps;
        }
        snprintf(assets.large_image, 128, "%s", song.isHighRes() ? "test" : HIFI_ASSET);
        snprintf(assets.large_text, 128, "%s", song.isHighRes() ? "Playing High-Res Audio" : "");
        if (song.id[0] != '\0') {
            struct DiscordActivitySecrets secrets{};
            memset(&secrets, 0, sizeof(secrets));
            snprintf(secrets.join, 128, "%s", song.id);
            activity.secrets = secrets;
        }
        activity.assets = assets;

        activity.instance = false;

        manager->update_activity(manager, &activity, nullptr, nullptr);
    } else {
        std::cout << "Clearing activity" << std::endl;
        manager->clear_activity(manager, nullptr, nullptr);
    }
}


static void discordInit() {
    memset(&app, 0, sizeof(app));

    IDiscordCoreEvents events;
    memset(&events, 0, sizeof(events));

    struct DiscordCreateParams params{};
    params.client_id = APPLICATION_ID;
    params.flags = DiscordCreateFlags_Default;
    params.events = &events;
    params.event_data = &app;

    DiscordCreate(DISCORD_VERSION, &params, &app.core);

    std::lock_guard<std::mutex> lock(currentSongMutex);
    currentStatus = "Connected to Discord";
}


[[noreturn]] inline void rpcLoop() {
    using json = nlohmann::json;
    using string = std::string;
    httplib::Client cli("api.tidal.com", 80, 3);
    char getSongInfoBuf[1024];
    json j;
    static Song curSong;

    for (;;) {
        if (isPresenceActive) {
            std::wstring tmpTrack, tmpArtist;
            auto localStatus = tidalInfo(tmpTrack, tmpArtist);

            // If song is playing
            if (localStatus == playing) {
                // if new song is playing
                if (rawWstringToString(tmpTrack) != curSong.title || rawWstringToString(tmpArtist) != curSong.artist) {
                    // assign new info to current track
                    curSong.title = rawWstringToString(tmpTrack);
                    curSong.artist = rawWstringToString(tmpArtist);

                    curSong.runtime = 0;
                    curSong.pausedtime = 0;
                    curSong.setQuality("");
                    curSong.id[0] = '\0';
                    curSong.loaded = true;

                    std::lock_guard<std::mutex> lock(currentSongMutex);
                    currentStatus = "Playing " + curSong.title;

                    // get info form TIDAL api
                    auto search_param =
                        std::string(curSong.title + " - " + curSong.artist.substr(0, curSong.artist.find('&')));

                    sprintf(getSongInfoBuf,
                            "/v1/search?query=%s&limit=50&offset=0&types=TRACKS&countryCode=%s",
                            urlEncode(search_param).c_str(), countryCode ? countryCode : "US");

                    std::clog << "Querying :" << getSongInfoBuf << "\n";

                    httplib::Headers headers = {{
                                                    "x-tidal-token", "CzET4vdadNUFQ5JU"
                                                }};
                    auto res = cli.Get(getSongInfoBuf, headers);

                    if (res && res->status == 200) {
                        try {
                            j = json::parse(res->body);
                            for (auto i = 0u; i < j["tracks"]["totalNumberOfItems"].get<unsigned>(); i++) {
                                // convert title from windows and from tidal api to strings, json lib doesn't support wide string
                                // so wstrings are pared as strings and have the same convention errors
                                auto fetched_str = j["tracks"]["items"][i]["title"].get<std::string>();
                                auto c_str = rawWstringToString(tmpTrack);

                                if (fetched_str == c_str) {
                                    if (curSong.runtime == 0
                                        or j["tracks"]["items"][i]["audioQuality"].get<std::string>().compare("HI_RES")
                                            == 0) {     // Ignore songs with same name if you have found song
                                        curSong.setQuality(j["tracks"]["items"][i]["audioQuality"].get<std::string>());
                                        curSong.trackNumber = j["tracks"]["items"][i]["trackNumber"].get<uint_fast8_t>();
                                        curSong.volumeNumber = j["tracks"]["items"][i]["volumeNumber"].get<uint_fast8_t>();
                                        curSong.runtime = j["tracks"]["items"][i]["duration"].get<int64_t>();
                                        sprintf(curSong.id, "%u", j["tracks"]["items"][i]["id"].get<unsigned>());
                                        if (curSong.isHighRes()) break;     // keep searching for high-res version.
                                    }
                                }
                            }
                        } catch (...) {
                            std::cerr << "Error getting info from api: " << curSong << "\n";
                        }
                    } else {
                        std::cout << "Did not get results\n";
                    }

#ifdef DEBUG
                    std::cout << curSong.title << "\tFrom: " << curSong.artist << std::endl;
#endif

                    // get time just before passing it to RPC handlers
                    curSong.starttime = CURRENT_TIME + 2;  // add 2 seconds to be more accurate, not a chance
                    updateDiscordPresence(curSong);
                } else {
                    if (curSong.isPaused) {
                        curSong.isPaused = false;
                        updateDiscordPresence(curSong);

                        std::lock_guard<std::mutex> lock(currentSongMutex);
                        currentStatus = "Paused " + curSong.title;
                    }
                }

            } else if (localStatus == opened) {
                curSong.pausedtime += 1;
                curSong.isPaused = true;
                updateDiscordPresence(curSong);

                std::lock_guard<std::mutex> lock(currentSongMutex);
                currentStatus = "Paused " + curSong.title;
            } else {
                curSong = Song();
                updateDiscordPresence(curSong);

                std::lock_guard<std::mutex> lock(currentSongMutex);
                currentStatus = "Waiting for Tidal";
            }
        } else {
            curSong = Song();
            updateDiscordPresence(curSong);

            std::lock_guard<std::mutex> lock(currentSongMutex);
            currentStatus = "Disabled";
        }

        enum EDiscordResult result = app.core->run_callbacks(app.core);
        if (result != DiscordResult_Ok) {
            std::cout << "Bad result " << result << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    }
}


int main(int argc, char **argv) {

    // get country code for TIDAL api queries
    countryCode = getLocale();
    isPresenceActive = true;

    // Qt main app setup
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":assets/icon.ico"));

    QSystemTrayIcon tray(QIcon(":assets/icon.ico"), &app);
    QAction titleAction(QIcon(":assets/icon.ico"), "TIDAL - Discord RPC ", nullptr);
    QAction changePresenceStatusAction("Running", nullptr);
    changePresenceStatusAction.setCheckable(true);
    changePresenceStatusAction.setChecked(true);
    QObject::connect(&changePresenceStatusAction,
                     &QAction::triggered,
                     [&changePresenceStatusAction]() {
                       isPresenceActive = !isPresenceActive;
                       changePresenceStatusAction.setText(isPresenceActive ? "Running"
                                                                           : "Disabled (click to re-enable)");
                     }
    );

    QAction quitAction("Exit", nullptr);
    QObject::connect(&quitAction, &QAction::triggered, [&app]() {
      updateDiscordPresence(Song());
      app.quit();
    });

    QAction currentlyPlayingAction("Status: waiting", nullptr);
    currentlyPlayingAction.setDisabled(true);

    QMenu trayMenu("TIDAL - RPC", nullptr);

    trayMenu.addAction(&titleAction);
    trayMenu.addAction(&changePresenceStatusAction);
    trayMenu.addAction(&currentlyPlayingAction);
    trayMenu.addAction(&quitAction);

    tray.setContextMenu(&trayMenu);

    tray.show();

    #if defined(__APPLE__) or defined(__MACH__)

    if (!macPerms()){
        std::cerr << "No Screen Recording Perms \n";
    }

    #endif

    QTimer timer(&app);
    QObject::connect(&timer, &QTimer::timeout, &app, [&currentlyPlayingAction]() {
      std::lock_guard<std::mutex> lock(currentSongMutex);
      currentlyPlayingAction.setText("Status: " + QString(currentStatus.c_str()));
    });
    timer.start(1000);

    QObject::connect(&app, &QApplication::aboutToQuit, [&timer]() {
      timer.stop();
    });

    discordInit();
    // RPC loop call
    std::thread t1(rpcLoop);
    t1.detach();

    return app.exec();

}
