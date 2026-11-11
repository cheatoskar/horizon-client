#include "path.h"

namespace nx {
namespace {

bool identChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::string stepText(const Step& s) {
    std::string t = s.member;
    if (s.index >= 0) t += "[" + std::to_string(s.index) + "]";
    return t;
}

} // namespace

std::string Path::text(int depth) const {
    std::string t = root;
    const size_t n = depth < 0 ? steps.size() : (static_cast<size_t>(depth) < steps.size() ? static_cast<size_t>(depth) : steps.size());
    for (size_t i = 0; i < n; i++) t += "." + stepText(steps[i]);
    return t;
}

Path Path::prefix(size_t depth) const {
    Path p = *this;
    if (depth < p.steps.size()) p.steps.resize(depth);
    return p;
}

Path Path::child(const std::string& member, int index, uintptr_t obj) const {
    Path p = *this;
    Step s;
    s.member = member;
    s.index = index;
    s.obj = obj;
    p.steps.push_back(s);
    return p;
}

bool Path::sameAs(const Path& o) const {
    if (root != o.root || steps.size() != o.steps.size()) return false;
    for (size_t i = 0; i < steps.size(); i++)
        if (steps[i].member != o.steps[i].member || steps[i].index != o.steps[i].index) return false;
    return true;
}

std::string Path::crumb(int step) const {
    if (step < 0) return root;
    if (static_cast<size_t>(step) >= steps.size()) return "";
    return stepText(steps[static_cast<size_t>(step)]);
}

uint32_t rootEngineId(const std::string& root) {
    if (root == "App") return 1;       // HORIZON_ENGINE_APP
    if (root == "Race") return 2;      // HORIZON_ENGINE_RACE
    if (root == "Challenge") return 3; // HORIZON_ENGINE_CHALLENGE
    return 0;
}

bool parsePath(const std::string& text, Path* out) {
    Path p;
    size_t i = 0;
    const size_t n = text.size();
    auto ident = [&](std::string* s) {
        const size_t b = i;
        while (i < n && identChar(text[i])) i++;
        *s = text.substr(b, i - b);
        return !s->empty();
    };
    if (!ident(&p.root) || !rootEngineId(p.root)) return false;
    while (i < n) {
        if (text[i] != '.') return false;
        i++;
        Step s;
        if (!ident(&s.member)) return false;
        if (i < n && text[i] == '[') {
            i++;
            const size_t b = i;
            long long v = 0;
            while (i < n && text[i] >= '0' && text[i] <= '9' && i - b < 9) v = v * 10 + (text[i++] - '0');
            if (i == b || i >= n || text[i] != ']') return false;
            i++;
            s.index = static_cast<int>(v);
        }
        p.steps.push_back(s);
        if (p.steps.size() > 64) return false;
    }
    *out = p;
    return true;
}

void Navigator::open(const Path& p) {
    if (!history_.empty() && history_[pos_].sameAs(p)) {
        history_[pos_] = p;
        return;
    }
    if (!history_.empty()) history_.resize(pos_ + 1);
    history_.push_back(p);
    if (history_.size() > kMaxHistory) history_.erase(history_.begin());
    pos_ = history_.size() - 1;
}

bool Navigator::back() {
    if (!canBack()) return false;
    pos_--;
    return true;
}

bool Navigator::forward() {
    if (!canForward()) return false;
    pos_++;
    return true;
}

bool Navigator::isPinned(const Path& p) const {
    for (const Path& q : pins_)
        if (q.sameAs(p)) return true;
    return false;
}

bool Navigator::pin(const Path& p) {
    if (p.empty() || isPinned(p) || pins_.size() >= kMaxPins) return false;
    pins_.push_back(p);
    return true;
}

void Navigator::unpin(size_t i) {
    if (i < pins_.size()) pins_.erase(pins_.begin() + static_cast<ptrdiff_t>(i));
}

std::string Navigator::pinsText() const {
    std::string t;
    for (const Path& p : pins_) {
        if (!t.empty()) t += "\n";
        t += p.text();
    }
    return t;
}

void Navigator::setPinsText(const std::string& text) {
    pins_.clear();
    size_t b = 0;
    while (b <= text.size()) {
        size_t e = text.find('\n', b);
        if (e == std::string::npos) e = text.size();
        Path p;
        if (e > b && parsePath(text.substr(b, e - b), &p)) pin(p);
        b = e + 1;
    }
}

} // namespace nx
