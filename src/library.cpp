#include "library.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

using nlohmann::json;
namespace fs = std::filesystem;

namespace tub {

const char* status_label(Status s) {
    switch (s) {
    case Status::Online: return "En ligne";
    case Status::Removed: return "Supprimée";
    case Status::Private: return "Privée";
    case Status::Error: return "Erreur";
    default: return "—";
    }
}

namespace {

template <class T> void get(const json& j, const char* k, T& v) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return;
    try { v = it->get<T>(); } catch (...) {}
}

} // namespace

bool load_library(const std::string& path, Library& out) {
    std::string s = read_file(path);
    if (s.empty()) return false;
    json j = json::parse(s, nullptr, false);
    if (!j.is_object()) return false;
    if (auto it = j.find("settings"); it != j.end() && it->is_object()) {
        Settings& c = out.cfg;
        const json& v = *it;
        get(v, "dest_dir", c.dest_dir); get(v, "cookies", c.cookies); get(v, "tools_dir", c.tools_dir);
        get(v, "format", c.format); get(v, "quality", c.quality); get(v, "subfolders", c.subfolders);
        get(v, "remove_done", c.remove_done); get(v, "pause_enabled", c.pause_enabled);
        get(v, "pause_every_min", c.pause_every_min); get(v, "pause_sec", c.pause_sec);
        get(v, "sort", c.sort); get(v, "sort_desc", c.sort_desc);
        get(v, "include_shorts", c.include_shorts); get(v, "include_live", c.include_live);
        get(v, "check_after_import", c.check_after_import);
    }
    get(j, "lists", out.lists);
    get(j, "next_order", out.next_order);
    if (auto it = j.find("videos"); it != j.end() && it->is_array()) {
        for (const json& v : *it) {
            if (!v.is_object()) continue;
            Video x;
            get(v, "id", x.id);
            if (x.id.empty()) continue;
            int st = 0;
            long long added = 0, checked = 0;
            get(v, "start", x.start); get(v, "title", x.title); get(v, "channel", x.channel);
            get(v, "channel_url", x.channel_url); get(v, "status", st); get(v, "added", added);
            get(v, "published", x.published);
            get(v, "checked", checked); get(v, "order", x.order); get(v, "lists", x.lists); get(v, "file", x.file);
            get(v, "arch_title", x.arch_title); get(v, "arch_channel", x.arch_channel);
            get(v, "arch_channel_url", x.arch_channel_url); get(v, "arch_date", x.arch_date);
            get(v, "arch_first", x.arch_first); get(v, "arch_last", x.arch_last); get(v, "arch_snaps", x.arch_snaps);
            get(v, "note", x.note);
            x.status = Status(st >= 0 && st <= 4 ? st : 0);
            x.added = time_t(added);
            x.checked = time_t(checked);
            out.videos.push_back(std::move(x));
        }
    }
    if (out.next_order <= int(out.videos.size())) out.next_order = int(out.videos.size()) + 1;
    return true;
}

std::string serialize_library(const Library& lib) {
    json j;
    const Settings& c = lib.cfg;
    j["settings"] = {{"dest_dir", c.dest_dir}, {"cookies", c.cookies}, {"tools_dir", c.tools_dir},
                     {"format", c.format}, {"quality", c.quality}, {"subfolders", c.subfolders},
                     {"remove_done", c.remove_done}, {"pause_enabled", c.pause_enabled},
                     {"pause_every_min", c.pause_every_min}, {"pause_sec", c.pause_sec}, {"sort", c.sort},
                     {"sort_desc", c.sort_desc}, {"include_shorts", c.include_shorts},
                     {"include_live", c.include_live}, {"check_after_import", c.check_after_import}};
    j["lists"] = lib.lists;
    j["next_order"] = lib.next_order;
    json arr = json::array();
    for (const Video& v : lib.videos) {
        json e = {{"id", v.id}, {"title", v.title}, {"channel", v.channel}, {"status", int(v.status)},
                  {"added", (long long)v.added}, {"order", v.order}, {"lists", v.lists}};
        if (v.start) e["start"] = v.start;
        if (!v.channel_url.empty()) e["channel_url"] = v.channel_url;
        if (!v.published.empty()) e["published"] = v.published;
        if (v.checked) e["checked"] = (long long)v.checked;
        if (!v.file.empty()) e["file"] = v.file;
        if (v.arch_snaps >= 0) {
            e["arch_snaps"] = v.arch_snaps;
            e["arch_title"] = v.arch_title;
            e["arch_channel"] = v.arch_channel;
            e["arch_channel_url"] = v.arch_channel_url;
            e["arch_date"] = v.arch_date;
            e["arch_first"] = v.arch_first;
            e["arch_last"] = v.arch_last;
        }
        if (!v.note.empty()) e["note"] = v.note;
        arr.push_back(std::move(e));
    }
    j["videos"] = std::move(arr);
    return j.dump(1);
}

bool write_file_atomic(const std::string& path, const std::string& data) {
    fs::path p = fs::u8path(path), tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
    }
    return !ec;
}

std::string read_file(const std::string& path, size_t max_bytes) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return {};
    std::string s;
    char buf[65536];
    while (f && s.size() < max_bytes) {
        f.read(buf, sizeof buf);
        s.append(buf, size_t(f.gcount()));
    }
    // Fichier texte en UTF-16 (Bloc-notes « Unicode ») : conversion en UTF-8.
    if (s.size() >= 2 && (unsigned char)s[0] == 0xFF && (unsigned char)s[1] == 0xFE) {
        std::string o;
        for (size_t i = 2; i + 1 < s.size(); i += 2) {
            unsigned c = (unsigned char)s[i] | ((unsigned char)s[i + 1] << 8);
            if (c < 0x80) o += char(c);
            else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 0x3F)); }
            else { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
        }
        return o;
    }
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

std::string list_name_from_path(const std::string& path) {
    std::string n = fs::u8path(path).stem().u8string();
    return n.empty() ? std::string("Import") : n;
}

} // namespace tub
