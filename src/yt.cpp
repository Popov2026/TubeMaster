#include "yt.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace yt {

namespace {

bool id_char(char c) { return std::isalnum((unsigned char)c) || c == '-' || c == '_'; }

bool url_stop(char c) {
    return (unsigned char)c <= ' ' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '`' || c == ')' ||
           c == ']' || c == '}' || c == ',';
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

// Valeur d'un paramètre d'URL (« v », « t »…).
std::string query_param(const std::string& url, const std::string& key) {
    size_t q = url.find_first_of("?#&");
    while (q != std::string::npos) {
        size_t a = q + 1;
        if (url.compare(a, key.size() + 1, key + "=") == 0) {
            size_t e = url.find_first_of("&#", a);
            return url.substr(a + key.size() + 1, e == std::string::npos ? std::string::npos : e - a - key.size() - 1);
        }
        q = url.find_first_of("&#", a);
    }
    return {};
}

// « 500s », « 1m30s », « 1h2m3s », « 90 » → secondes.
int parse_time(const std::string& t) {
    if (t.empty()) return 0;
    int total = 0, cur = 0;
    bool any = false;
    for (char c : t) {
        if (std::isdigit((unsigned char)c)) { cur = cur * 10 + (c - '0'); any = true; }
        else if (c == 'h') { total += cur * 3600; cur = 0; }
        else if (c == 'm') { total += cur * 60; cur = 0; }
        else if (c == 's') { total += cur; cur = 0; }
        else break;
    }
    return any ? total + cur : 0;
}

std::string take_id(const std::string& s, size_t pos) {
    if (pos > s.size() || s.size() - pos < 11) return {};
    std::string id = s.substr(pos, 11);
    if (!valid_id(id)) return {};
    if (pos + 11 < s.size() && id_char(s[pos + 11])) return {};   // plus long : pas un identifiant
    return id;
}

void append_utf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
}

std::string html_unescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 10) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") o += '&';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "nbsp") o += ' ';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? unsigned(std::strtoul(ent.c_str() + 2, nullptr, 16))
                                                                             : unsigned(std::strtoul(ent.c_str() + 1, nullptr, 10));
            if (cp) append_utf8(o, cp);
        } else { o += s.substr(i, e - i + 1); }
        i = e;
    }
    return o;
}

// Chaîne JSON qui commence en « pos » (juste après le guillemet ouvrant).
std::string json_string_at(const std::string& s, size_t pos) {
    std::string o;
    for (size_t i = pos; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') return o;
        if (c != '\\' || i + 1 >= s.size()) { o += c; continue; }
        char n = s[++i];
        switch (n) {
        case 'n': o += '\n'; break;
        case 't': o += '\t'; break;
        case 'r': break;
        case 'u': {
            if (i + 4 >= s.size()) return o;
            unsigned cp = unsigned(std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
                unsigned lo = unsigned(std::strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16));
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 6;
            }
            append_utf8(o, cp);
            break;
        }
        default: o += n; break;
        }
    }
    return o;
}

// "clé":"valeur" dans un texte JSON embarqué (première occurrence non vide).
std::string json_field(const std::string& html, const char* key) {
    std::string pat = std::string("\"") + key + "\":\"";
    for (size_t p = html.find(pat); p != std::string::npos; p = html.find(pat, p + 1)) {
        std::string v = json_string_at(html, p + pat.size());
        if (!trim(v).empty()) return v;
    }
    return {};
}

// <meta property|name|itemprop="…" content="…"> (attributs dans n'importe quel ordre).
std::string meta_content(const std::string& html, const std::string& attr_value) {
    std::string l = lower(html), needle = "\"" + lower(attr_value) + "\"";
    for (size_t p = l.find(needle); p != std::string::npos; p = l.find(needle, p + 1)) {
        size_t tag = l.rfind('<', p), end = l.find('>', p);
        if (tag == std::string::npos || end == std::string::npos) continue;
        if (l.compare(tag, 5, "<meta") != 0 && l.compare(tag, 5, "<link") != 0) continue;
        size_t c = l.find("content=\"", tag);
        if (c == std::string::npos || c > end) continue;
        size_t a = c + 9, b = html.find('"', a);
        if (b == std::string::npos || b > end) continue;
        std::string v = trim(html_unescape(html.substr(a, b - a)));
        if (!v.empty()) return v;
    }
    return {};
}

// Texte du premier élément dont l'attribut class contient « cls » (<span class="… cls …">texte</span>).
std::string class_text(const std::string& html, const std::string& cls) {
    std::string l = lower(html);
    for (size_t p = l.find(cls); p != std::string::npos; p = l.find(cls, p + 1)) {
        size_t tag = l.rfind('<', p);
        if (tag == std::string::npos) continue;
        size_t cattr = l.rfind("class=\"", p);
        if (cattr == std::string::npos || cattr < tag) continue;
        size_t gt = l.find('>', p);
        if (gt == std::string::npos) continue;
        size_t end = l.find('<', gt + 1);
        if (end == std::string::npos) continue;
        std::string v = trim(html_unescape(html.substr(gt + 1, end - gt - 1)));
        if (!v.empty()) return v;
    }
    return {};
}

std::string strip_youtube_suffix(std::string t) {
    for (const char* suf : {" - YouTube", " – YouTube"})
        if (t.size() > std::strlen(suf) && t.compare(t.size() - std::strlen(suf), std::string::npos, suf) == 0)
            t.resize(t.size() - std::strlen(suf));
    return trim(t);
}

} // namespace

// ── liens ───────────────────────────────────────────────────────────────────
bool valid_id(const std::string& id) {
    if (id.size() != 11) return false;
    for (char c : id) if (!id_char(c)) return false;
    return true;
}

std::string extract_id(const std::string& text, int* start_sec) {
    std::string s = trim(text);
    if (start_sec) *start_sec = 0;
    if (valid_id(s)) return s;
    std::string l = lower(s);
    if (l.find("youtu") == std::string::npos) return {};
    std::string id;
    size_t p;
    if ((p = l.find("youtu.be/")) != std::string::npos) id = take_id(s, p + 9);
    if (id.empty()) {
        for (const char* seg : {"/shorts/", "/embed/", "/live/", "/v/", "/e/"}) {
            if ((p = l.find(seg)) != std::string::npos && l.find("youtu") < p) { id = take_id(s, p + std::strlen(seg)); break; }
        }
    }
    if (id.empty()) {
        std::string v = query_param(s, "v");
        if (valid_id(v)) id = v;
    }
    if (!id.empty() && start_sec) {
        std::string t = query_param(s, "t");
        if (t.empty()) t = query_param(s, "start");
        *start_sec = parse_time(t);
    }
    return id;
}

std::vector<Link> find_links(const std::string& text) {
    std::vector<Link> out;
    std::string l = lower(text);
    size_t line_start = 0;
    // Identifiants seuls sur leur ligne.
    auto bare_line = [&](size_t a, size_t b) {
        std::string line = trim(text.substr(a, b - a));
        if (valid_id(line)) out.push_back({line, 0});
    };
    std::vector<std::pair<size_t, Link>> found;
    for (size_t p = l.find("youtu"); p != std::string::npos;) {
        size_t a = p, b = p;
        while (a > 0 && !url_stop(text[a - 1])) --a;
        while (b < text.size() && !url_stop(text[b])) ++b;
        int st = 0;
        std::string id = extract_id(text.substr(a, b - a), &st);
        if (!id.empty()) found.push_back({a, {id, st}});
        p = l.find("youtu", std::max(b, p + 5));
    }
    // Fusion dans l'ordre du texte : liens et identifiants seuls.
    size_t fi = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            while (fi < found.size() && found[fi].first < i) out.push_back(found[fi++].second);
            if (l.substr(line_start, i - line_start).find("youtu") == std::string::npos) bare_line(line_start, i);
            line_start = i + 1;
        }
    }
    while (fi < found.size()) out.push_back(found[fi++].second);
    return out;
}

std::string watch_url(const std::string& id, int start) {
    std::string u = "https://www.youtube.com/watch?v=" + id;
    if (start > 0) u += "&t=" + std::to_string(start) + "s";
    return u;
}

std::string thumb_url(const std::string& id) { return "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg"; }

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string channel_url(const std::string& input) {
    std::string s = trim(input);
    if (s.empty()) return {};
    if (lower(s).rfind("http", 0) == 0) return s;
    if (lower(s).find("youtube.com/") != std::string::npos) return "https://" + s;
    if (s[0] == '@') return "https://www.youtube.com/" + s;
    if (s.size() == 24 && s.compare(0, 2, "UC") == 0) return "https://www.youtube.com/channel/" + s;
    return "https://www.youtube.com/@" + s;
}

// ── oEmbed ──────────────────────────────────────────────────────────────────
std::string oembed_url(const std::string& id) {
    return "https://www.youtube.com/oembed?format=json&url=" + url_encode(watch_url(id));
}

bool parse_oembed(const std::string& body, OEmbed& out) {
    json j = json::parse(body, nullptr, false);
    if (!j.is_object()) return false;
    auto str = [&](const char* k) { auto it = j.find(k); return it != j.end() && it->is_string() ? it->get<std::string>() : std::string(); };
    out.title = str("title");
    out.author = str("author_name");
    out.author_url = str("author_url");
    return !out.title.empty();
}

// ── archives ────────────────────────────────────────────────────────────────
std::string wayback_cdx_url(const std::string& id) {
    return "https://web.archive.org/cdx/search/cdx?url=youtube.com/watch%3Fv%3D" + id +
           "&matchType=prefix&output=json&fl=timestamp,statuscode&filter=statuscode:200&limit=300";
}

std::vector<std::string> parse_cdx(const std::string& body) {
    std::vector<std::string> out;
    json j = json::parse(body, nullptr, false);
    if (!j.is_array()) return out;
    for (size_t i = 0; i < j.size(); ++i) {
        const json& r = j[i];
        if (!r.is_array() || r.empty() || !r[0].is_string()) continue;
        std::string ts = r[0].get<std::string>();
        if (ts == "timestamp" || ts.size() < 8) continue;
        if (r.size() > 1 && r[1].is_string() && r[1].get<std::string>() != "200") continue;
        out.push_back(ts);
    }
    return out;
}

std::string wayback_raw_url(const std::string& ts, const std::string& id) {
    return "https://web.archive.org/web/" + ts + "id_/https://www.youtube.com/watch?v=" + id;
}
std::string wayback_page_url(const std::string& ts, const std::string& id) {
    return "https://web.archive.org/web/" + ts + "/https://www.youtube.com/watch?v=" + id;
}
std::string wayback_video_url(const std::string& id) {
    return "https://web.archive.org/web/2oe_/http://wayback-fakeurl.archive.org/yt/" + id;
}
std::string wayback_calendar_url(const std::string& id) {
    return "https://web.archive.org/web/*/https://www.youtube.com/watch?v=" + id + "*";
}

std::string fmt_ts(const std::string& ts) {
    if (ts.size() < 8) return ts;
    std::string r = ts.substr(6, 2) + "/" + ts.substr(4, 2) + "/" + ts.substr(0, 4);
    if (ts.size() >= 12) r += " " + ts.substr(8, 2) + ":" + ts.substr(10, 2);
    return r;
}

std::string normalize_date(const std::string& text) {
    std::string s = trim(text);
    if (s.size() >= 10 && std::isdigit((unsigned char)s[0]) && s[4] == '-' && s[7] == '-') return s.substr(0, 10);
    static const char* months[][3] = {{"jan", "janv", "janvier"}, {"feb", "févr", "février"}, {"mar", "mars", "mars"},
                                      {"apr", "avr", "avril"},   {"may", "mai", "mai"},       {"jun", "juin", "juin"},
                                      {"jul", "juil", "juillet"}, {"aug", "août", "aout"},     {"sep", "sept", "septembre"},
                                      {"oct", "oct", "octobre"}, {"nov", "nov", "novembre"}, {"dec", "déc", "décembre"}};
    // Mots, nombres : on cherche un mois, un jour (1-31) et une année (4 chiffres).
    std::vector<std::string> tok;
    std::string cur;
    for (char c : lower(s)) {
        if (std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80) cur += c;
        else { if (!cur.empty()) tok.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) tok.push_back(cur);
    int day = 0, month = 0, year = 0;
    for (auto& t : tok) {
        if (std::isdigit((unsigned char)t[0])) {
            int v = std::atoi(t.c_str());
            if (t.size() == 4 && v > 1990) year = v;
            else if (v >= 1 && v <= 31 && !day) day = v;
            continue;
        }
        for (int m = 0; m < 12 && !month; ++m)
            for (const char* name : months[m])
                if (t == name || (t.size() >= 3 && std::strncmp(t.c_str(), name, t.size()) == 0 && t.size() <= std::strlen(name))) {
                    month = m + 1;
                    break;
                }
    }
    if (!day || !month || !year) return {};
    char b[16];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", year, month, day);
    return b;
}

bool parse_watch_page(const std::string& html, Archived& out) {
    // Titre : balises meta (toutes époques), sinon les données du lecteur, sinon <title>.
    std::string t = meta_content(html, "og:title");
    if (t.empty()) t = meta_content(html, "title");
    if (t.empty()) {
        std::string v = json_field(html, "title");
        if (!v.empty()) t = v;
    }
    if (t.empty()) {
        std::string l = lower(html);
        size_t a = l.find("<title"), b = l.find("</title>");
        if (a != std::string::npos && b != std::string::npos && b > a) {
            a = l.find('>', a);
            if (a != std::string::npos && a < b) t = trim(html_unescape(html.substr(a + 1, b - a - 1)));
        }
    }
    t = strip_youtube_suffix(t);
    if (t == "YouTube" || lower(t).find("unavailable") != std::string::npos) t.clear();
    out.title = t;
    // Chaîne.
    out.channel = json_field(html, "ownerChannelName");
    if (out.channel.empty()) out.channel = json_field(html, "author");
    if (out.channel.empty()) {
        // Anciennes pages : <span itemprop="author" …><link itemprop="name" content="…">
        size_t a = lower(html).find("itemprop=\"author\"");
        if (a != std::string::npos) out.channel = meta_content(html.substr(a, 600), "name");
    }
    if (out.channel.empty()) out.channel = class_text(html, "yt-user-name");     // 2010-2016
    if (out.channel.empty()) out.channel = class_text(html, "watch-uploader-info");
    if (out.channel.empty()) out.channel = class_text(html, "contributor");     // 2006-2009
    std::string cid = json_field(html, "channelId");
    if (!cid.empty()) out.channel_url = "https://www.youtube.com/channel/" + cid;
    else {
        size_t a = lower(html).find("itemprop=\"author\"");
        if (a != std::string::npos) {
            std::string seg = html.substr(a, 600), sl = lower(seg);
            size_t h = sl.find("href=\"");
            if (h != std::string::npos) { size_t e = seg.find('"', h + 6); if (e != std::string::npos) out.channel_url = seg.substr(h + 6, e - h - 6); }
        }
    }
    // Date, description, vues.
    out.date = meta_content(html, "datePublished");
    if (out.date.empty()) out.date = meta_content(html, "uploadDate");
    if (out.date.empty()) out.date = json_field(html, "publishDate");
    if (out.date.empty()) out.date = json_field(html, "uploadDate");
    if (out.date.empty()) {
        // Page actuelle sans microformat (vérification « robot ») : date affichée sous la vidéo.
        static const std::string key = "\"dateText\":{\"simpleText\":\"";
        size_t p = html.find(key);
        if (p != std::string::npos) out.date = json_string_at(html, p + key.size());
    }
    if (out.date.empty()) out.date = class_text(html, "watch-time-text");      // « Publiée le 23 avr. 2005 »
    if (out.date.empty()) out.date = class_text(html, "watch-video-date");
    if (out.date.empty()) out.date = class_text(html, "eow-date");
    for (const char* pre : {"Published on ", "Uploaded on ", "Publiée le ", "Ajoutée le ", "Mise en ligne le "})
        if (out.date.rfind(pre, 0) == 0) out.date = out.date.substr(std::strlen(pre));
    if (out.date.size() > 10 && out.date[4] == '-') out.date.resize(10);
    out.description = json_field(html, "shortDescription");
    if (out.description.empty()) out.description = meta_content(html, "og:description");
    if (out.description.empty()) out.description = meta_content(html, "description");
    out.views = meta_content(html, "interactionCount");
    if (out.views.empty()) out.views = json_field(html, "viewCount");
    if (out.views.empty()) {
        std::string v = class_text(html, "watch-view-count");
        for (char ch : v) if (std::isdigit((unsigned char)ch)) out.views += ch;
    }
    return !out.title.empty();
}

std::string filmot_url(const std::string& id) { return "https://filmot.com/search/" + url_encode(id) + "/1"; }
std::string filmot_title_url(const std::string& q) { return "https://filmot.com/search/" + url_encode(q) + "/1"; }
std::string ghostarchive_url(const std::string& id) { return "https://ghostarchive.org/search?term=" + url_encode(id); }
std::string archive_today_url(const std::string& id) { return "https://archive.ph/" + watch_url(id); }
std::string google_url(const std::string& q) { return "https://www.google.com/search?q=" + url_encode(q); }
std::string bing_url(const std::string& q) { return "https://www.bing.com/search?q=" + url_encode(q); }
std::string duckduckgo_url(const std::string& q) { return "https://duckduckgo.com/?q=" + url_encode(q); }
std::string youtube_search_url(const std::string& q) { return "https://www.youtube.com/results?search_query=" + url_encode(q); }

// ── yt-dlp ──────────────────────────────────────────────────────────────────
const char* format_name(Format f) { return f == Format::MKV ? "MKV" : f == Format::MP4 ? "MP4" : "MP3"; }
const char* quality_name(Quality q, Format f) {
    if (f == Format::MP3) return q == Quality::Max ? "qualité max" : "192 kb/s";
    return q == Quality::Max ? "taille maxi" : "720p";
}

std::vector<std::string> download_args(const Job& j) {
    std::vector<std::string> a = {"--newline", "--no-playlist", "--encoding", "utf-8", "--no-simulate",
                                  "--print", std::string("after_move:") + kFileMark + "%(filepath)s"};
#ifdef _WIN32
    a.push_back("--windows-filenames");
#endif
    if (!j.ffmpeg_dir.empty()) { a.push_back("--ffmpeg-location"); a.push_back(j.ffmpeg_dir); }
    if (!j.cookies.empty()) { a.push_back("--cookies"); a.push_back(j.cookies); }
    if (!j.js_runtime.empty()) { a.push_back("--js-runtimes"); a.push_back(j.js_runtime); }
    bool hd = j.quality == Quality::P720;
    switch (j.format) {
    case Format::MKV:
        a.insert(a.end(), {"-f", hd ? "bv*[height<=720]+ba/b[height<=720]/bv*+ba/b" : "bv*+ba/b",
                           "--merge-output-format", "mkv", "--remux-video", "mkv"});
        break;
    case Format::MP4:
        a.insert(a.end(), {"-f", hd ? "bv*[height<=720][ext=mp4]+ba[ext=m4a]/bv*[height<=720]+ba/b[height<=720]/b"
                                    : "bv*[ext=mp4]+ba[ext=m4a]/bv*+ba/b",
                           "--merge-output-format", "mp4", "--remux-video", "mp4"});
        break;
    case Format::MP3:
        a.insert(a.end(), {"-f", "ba/b", "-x", "--audio-format", "mp3", "--audio-quality", hd ? "192K" : "0"});
        break;
    }
    std::string dir = j.dest_dir;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    a.push_back("-o");
    a.push_back(dir + "%(title)s.%(ext)s");
    a.push_back(j.url);
    return a;
}

bool parse_progress(const std::string& line, Progress& p) {
    if (line.find("[download]") == std::string::npos) return false;
    size_t pc = line.find('%');
    if (pc == std::string::npos) return false;
    size_t a = pc;
    while (a > 0 && (std::isdigit((unsigned char)line[a - 1]) || line[a - 1] == '.')) --a;
    if (a == pc) return false;
    p.pct = std::atof(line.substr(a, pc - a).c_str());
    size_t at = line.find(" at ");
    if (at != std::string::npos) {
        size_t s = line.find_first_not_of(' ', at + 4), e = line.find(' ', s);
        if (s != std::string::npos) p.speed = line.substr(s, e == std::string::npos ? std::string::npos : e - s);
    }
    size_t eta = line.find("ETA ");
    if (eta != std::string::npos) p.eta = trim(line.substr(eta + 4, 9));
    return true;
}

std::vector<std::string> channel_args(const std::string& url) {
    return {"--flat-playlist", "--encoding", "utf-8", "--ignore-errors", "--print", "%(id)s\t%(title)s", url};
}

bool parse_channel_line(const std::string& line, std::string& id, std::string& title) {
    size_t t = line.find('\t');
    if (t == std::string::npos) return false;
    id = trim(line.substr(0, t));
    title = trim(line.substr(t + 1));
    if (title == "NA") title.clear();
    return valid_id(id);
}

} // namespace yt
