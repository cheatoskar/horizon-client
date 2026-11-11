#include "splits.h"

#include <cstdio>

#include "common/json.h"

namespace wrh {

std::vector<int> sectors(const std::vector<int>& splits) {
    std::vector<int> out;
    int prev = 0;
    for (int t : splits) {
        out.push_back(t >= 0 && prev >= 0 ? t - prev : -1);
        prev = t;
    }
    return out;
}

int sumOfBest(const MapStats& s) {
    if (s.golds.empty()) return -1;
    long long sum = 0;
    for (int g : s.golds) {
        if (g <= 0) return -1;
        sum += g;
    }
    return static_cast<int>(sum);
}

FinishResult applyFinish(MapStats& s, const std::vector<int>& splits, int respawns, long long now) {
    FinishResult r;
    if (splits.empty() || splits.back() <= 0) return r;
    s.finishes++;
    RunRecord rec;
    rec.at = now;
    rec.time = splits.back();
    rec.splits = splits;
    rec.respawns = respawns;
    s.runs.push_back(rec);
    while (s.runs.size() > kMaxRuns) s.runs.erase(s.runs.begin());
    r.gold.assign(splits.size(), false);
    if (s.pb.empty()) {
        r.firstRun = true;
        r.pb = true;
        s.pb = splits;
        s.golds = sectors(splits);
        for (size_t i = 0; i < r.gold.size(); i++) r.gold[i] = true;
        return r;
    }
    if (splits.size() != s.pb.size()) return r; // another layout: history only
    const std::vector<int> sec = sectors(splits);
    if (s.golds.size() != sec.size()) s.golds = sec;
    for (size_t i = 0; i < sec.size(); i++) {
        if (sec[i] > 0 && (s.golds[i] <= 0 || sec[i] < s.golds[i])) {
            s.golds[i] = sec[i];
            r.gold[i] = true;
        }
    }
    if (splits.back() < s.pb.back()) {
        s.pb = splits;
        r.pb = true;
    }
    return r;
}

int splitCount(const std::vector<int>& live, const MapStats& s, const Comparison& target) {
    size_t n = s.pb.size();
    if (!target.splits.empty() && (n == 0 || target.splits.size() == n)) n = target.splits.size();
    if (n == 0) n = target.splits.size();
    if (live.size() > n) n = live.size();
    return static_cast<int>(n);
}

std::vector<Row> buildRows(const std::vector<int>& live, const MapStats& s, const Comparison& target) {
    const int n = splitCount(live, s, target);
    std::vector<Row> rows;
    const bool pbFits = !s.pb.empty() && static_cast<int>(s.pb.size()) == n;
    const bool tFits = !target.splits.empty() && static_cast<int>(target.splits.size()) == n;
    int prevPb = kNoDelta, prevT = kNoDelta;
    int prevTime = 0;
    for (int i = 0; i < n; i++) {
        Row r;
        r.index = i;
        r.finish = i == n - 1;
        r.time = i < static_cast<int>(live.size()) ? live[static_cast<size_t>(i)] : -1;
        r.pbSplit = pbFits ? s.pb[static_cast<size_t>(i)] : -1;
        r.targetSplit = tFits ? target.splits[static_cast<size_t>(i)] : -1;
        r.prevDPb = prevPb;
        r.prevDTarget = prevT;
        if (r.time >= 0 && r.pbSplit >= 0) r.dPb = r.time - r.pbSplit;
        if (r.time >= 0 && r.targetSplit >= 0) r.dTarget = r.time - r.targetSplit;
        if (pbFits && static_cast<size_t>(i) < s.golds.size()) r.gold = s.golds[static_cast<size_t>(i)];
        if (r.time >= 0 && prevTime >= 0) {
            r.sector = r.time - prevTime;
            r.isGold = r.gold > 0 && r.sector < r.gold;
        }
        prevTime = r.time;
        if (r.dPb != kNoDelta) prevPb = r.dPb;
        if (r.dTarget != kNoDelta) prevT = r.dTarget;
        rows.push_back(r);
    }
    return rows;
}

int runningDelta(const std::vector<int>& live, int nowMs, const std::vector<int>& cmp) {
    const size_t next = live.size();
    if (nowMs < 0 || next >= cmp.size()) return kNoDelta;
    const int split = cmp[next];
    return nowMs > split ? nowMs - split : kNoDelta;
}

namespace {
std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += static_cast<char>(c);
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else {
            o += static_cast<char>(c);
        }
    }
    return o;
}
std::string list(const std::vector<int>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); i++) o += (i ? "," : "") + std::to_string(v[i]);
    return o + "]";
}
std::vector<int> ints(const horizon::Json* j) {
    std::vector<int> v;
    if (!j || j->type != horizon::Json::Array) return v;
    for (const horizon::Json& e : j->arr) v.push_back(static_cast<int>(e.asInt(-1)));
    return v;
}
} // namespace

std::string toJson(const MapStats& s) {
    std::string o = "{\n  \"format\": 1,\n  \"key\": \"" + esc(s.key) + "\",\n  \"uid\": \"" + esc(s.uid) + "\",\n  \"name\": \"" + esc(s.name) +
                    "\",\n  \"attempts\": " + std::to_string(s.attempts) + ",\n  \"resets\": " + std::to_string(s.resets) +
                    ",\n  \"finishes\": " + std::to_string(s.finishes) + ",\n  \"pb\": " + list(s.pb) + ",\n  \"golds\": " + list(s.golds) +
                    ",\n  \"runs\": [";
    for (size_t i = 0; i < s.runs.size(); i++) {
        const RunRecord& r = s.runs[i];
        o += std::string(i ? "," : "") + "\n    {\"at\": " + std::to_string(r.at) + ", \"time\": " + std::to_string(r.time) +
             ", \"respawns\": " + std::to_string(r.respawns) + ", \"splits\": " + list(r.splits) + "}";
    }
    return o + "\n  ]\n}\n";
}

bool fromJson(const std::string& text, MapStats* s) {
    horizon::Json j;
    std::string err;
    if (!horizon::JsonParser::parse(text, j, err) || j.type != horizon::Json::Object) return false;
    MapStats out;
    if (const horizon::Json* v = j.get("key")) out.key = v->asString();
    if (const horizon::Json* v = j.get("uid")) out.uid = v->asString();
    if (const horizon::Json* v = j.get("name")) out.name = v->asString();
    if (const horizon::Json* v = j.get("attempts")) out.attempts = static_cast<int>(v->asInt(0));
    if (const horizon::Json* v = j.get("resets")) out.resets = static_cast<int>(v->asInt(0));
    if (const horizon::Json* v = j.get("finishes")) out.finishes = static_cast<int>(v->asInt(0));
    out.pb = ints(j.get("pb"));
    out.golds = ints(j.get("golds"));
    if (const horizon::Json* runs = j.get("runs")) {
        if (runs->type == horizon::Json::Array)
            for (const horizon::Json& r : runs->arr) {
                RunRecord rec;
                if (const horizon::Json* v = r.get("at")) rec.at = v->asInt(0);
                if (const horizon::Json* v = r.get("time")) rec.time = static_cast<int>(v->asInt(-1));
                if (const horizon::Json* v = r.get("respawns")) rec.respawns = static_cast<int>(v->asInt(-1));
                rec.splits = ints(r.get("splits"));
                if (rec.time > 0) out.runs.push_back(rec);
            }
    }
    if (out.golds.size() != out.pb.size()) out.golds = sectors(out.pb);
    *s = out;
    return true;
}

std::string fileKey(const std::string& key) {
    std::string o;
    for (char c : key) {
        if (o.size() >= 64) break;
        o += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') ? c : '_';
    }
    return o.empty() ? std::string("map") : o;
}

std::string localKey(const std::string& name, int authorTime) {
    uint32_t h = 2166136261u;
    const std::string k = name + "|" + std::to_string(authorTime);
    for (unsigned char c : k) {
        h ^= c;
        h *= 16777619u;
    }
    char b[24];
    snprintf(b, sizeof(b), "local-%08x", h);
    return b;
}

} // namespace wrh
