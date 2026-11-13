// TMX Browser: the plugin's own HTTP client for the TMX exchanges (WinHTTP, worker threads).
//
// Two lanes. "api" (search, random, map files, Forever play links): one worker, requests at least 1 s apart,
// like TM Horizon does. "image" (thumbnails): three workers, starts at least 250 ms apart, with a disk cache
// (horizon\data\tmx-browser\thumbs\) and the decoding (stb_image, shrunk to 256 px) on the worker, so the
// game thread only creates the texture. Results are picked up on the game thread with poll().
// Only https to the five exchange hosts, also after redirects; user agent TM-Horizon/<version>.
#pragma once

#include <stdint.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tmx {

struct NetResponse {
    int status = 0;           // HTTP status; 0 no connection, -2 timeout, -3 refused (host not allowed, too large)
    std::string type;         // Content-Type without parameters
    std::string location;     // Location header (redirects not followed)
    std::string body;
};

enum class Lane { Api = 0, Image = 1 };

struct NetJob {
    uint64_t id = 0;
    Lane lane = Lane::Api;
    std::string url;
    size_t maxBytes = 0;
    bool follow = true;       // follow redirects (false: hand back the Location)
    bool decode = false;      // image: decode to RGBA (max 256 px wide)
    std::string cacheFile;    // image: full path of the cache file ("" = no cache)
    std::string sendLink;     // not empty: no HTTP, hand this tmtp:// link to the game window instead
    void* window = nullptr;   // sendLink: the game window (found on the game thread)
};

struct NetResult {
    uint64_t id = 0;
    NetResponse r;
    bool decoded = false;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    bool fromCache = false;
    bool placeholder = false; // TMX's "no picture" image
};

// Test seams: a fetcher instead of WinHTTP, a link sender instead of the game window.
using Fetcher = std::function<NetResponse(const std::string& url, size_t maxBytes, bool follow)>;
using LinkSender = std::function<bool(const std::string& link)>;

class Net {
public:
    ~Net() { stop(); }
    void configure(const std::string& userAgent, int apiGapMs, int imageGapMs);
    void setFetcher(Fetcher f) { fetcher_ = std::move(f); }
    void setLinkSender(LinkSender s) { linkSender_ = std::move(s); }
    // sync: submit() runs the job at once on the calling thread (tests); poll() hands the result back.
    void setSync(bool on) { sync_ = on; }
    void submit(const NetJob& job);
    bool poll(NetResult& out);
    void cancelAll();               // drops queued jobs; running ones finish and are thrown away
    void stop();                    // ends the threads (Shutdown)
    size_t queued() const;

private:
    void worker(Lane lane);
    NetResult run(const NetJob& job);
    NetResponse fetch(const std::string& url, size_t maxBytes, bool follow);
    bool sendLink(const std::string& link, void* window);

    std::string ua_ = "TM-Horizon";
    int gapMs_[2] = {1000, 250};
    uint64_t nextStart_[2] = {0, 0};
    Fetcher fetcher_;
    LinkSender linkSender_;
    bool sync_ = false;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<NetJob> q_[2];
    std::deque<NetResult> done_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
    unsigned gen_ = 1;
    void* session_ = nullptr;       // HINTERNET, shared by the workers
};

// Deletes the oldest cache files when there are more than keep (game start, worker thread).
void pruneCache(const std::string& folder, size_t keep);
// The game's main window (the Direct3D focus window when the device is known).
void* gameWindow(void* d3dDevice);

} // namespace tmx
