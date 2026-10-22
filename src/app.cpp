#include "app.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <filesystem>

#include <SDL.h>

#include "platform.hpp"

namespace fs = std::filesystem;

namespace sw {

namespace {

bool file_exists(const std::string& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(fs::u8path(p), ec);
}

#ifdef _WIN32
constexpr const char* kExe = ".exe";
constexpr char kPathSep = ';';
#else
constexpr const char* kExe = "";
constexpr char kPathSep = ':';
#endif

} // namespace

const unsigned int kBaseGlyphRanges[] = {0x0020, 0x024F, 0x0370, 0x03FF, 0x0400, 0x052F, 0x1E00, 0x1EFF,
                                         0x2000, 0x206F, 0x20A0, 0x20CF, 0x2100, 0x214F, 0x2190, 0x23FF,
                                         0x2500, 0x257F, 0x25A0, 0x27BF, 0};

static bool base_glyph(unsigned c) {
    for (int i = 0; kBaseGlyphRanges[i]; i += 2)
        if (c >= kBaseGlyphRanges[i] && c <= kBaseGlyphRanges[i + 1]) return true;
    return c < 0x20;
}

// Parcourt les textes affichés (titres, chaînes, journal…) : tout caractère inconnu
// des polices déclenche une reconstruction de l'atlas (polices de secours de Windows).
void App::scan_glyphs() {
    std::vector<unsigned> found;
    auto scan = [&](const std::string& s) {
        for (size_t i = 0; i < s.size();) {
            unsigned char c = (unsigned char)s[i];
            unsigned cp;
            int n;
            if (c < 0x80) { ++i; continue; }
            else if ((c >> 5) == 6) { cp = c & 0x1F; n = 1; }
            else if ((c >> 4) == 14) { cp = c & 0x0F; n = 2; }
            else if ((c >> 3) == 30) { cp = c & 0x07; n = 3; }
            else { ++i; continue; }
            if (i + size_t(n) >= s.size()) break;           // séquence tronquée
            for (int k = 1; k <= n; ++k) cp = (cp << 6) | ((unsigned char)s[i + size_t(k)] & 0x3F);
            i += size_t(n) + 1;
            if (!base_glyph(cp) && cp != 0xFE0F && cp != 0x200D) found.push_back(cp);
        }
    };
    for (auto& v : lib_.videos) { scan(v.title); scan(v.channel); scan(v.arch_title); scan(v.arch_channel); }
    for (auto& l : lib_.lists) scan(l);
    for (auto& j : jobs_) scan(j.title);
    for (auto& l : log_) scan(l);
    size_t n = 0;
    for (auto it = console_.rbegin(); it != console_.rend() && n < 200; ++it, ++n) scan(*it);
    if (dead_) { scan(dead_->found.title); scan(dead_->found.channel); scan(dead_->found.description); scan(dead_->oembed.title); }
    if (found.empty()) return;
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    std::vector<unsigned> merged;
    std::set_union(glyphs_.begin(), glyphs_.end(), found.begin(), found.end(), std::back_inserter(merged));
    if (merged.size() != glyphs_.size() && merged.size() < 12000) {
        glyphs_ = std::move(merged);
        glyphs_dirty_ = true;
    }
}

bool App::take_font_request(std::vector<unsigned>& extra) {
    if (!glyphs_dirty_) return false;
    glyphs_dirty_ = false;
    extra = glyphs_;
    diag("polices : " + std::to_string(glyphs_.size()) + " caractère(s) supplémentaire(s)");
    return true;
}

App::App(SDL_Window* win, const Fonts& fonts, float ui_scale) : win_(win), fonts_(fonts), scale_(ui_scale) {
    base_dir_ = exe_dir();
    data_path_ = (fs::u8path(base_dir_) / "tubemaster_data.json").u8string();
    diag_open((fs::u8path(base_dir_) / "tubemaster_log.txt").u8string());
    tub::load_library(data_path_, lib_);
    if (lib_.cfg.dest_dir.empty()) lib_.cfg.dest_dir = (fs::u8path(base_dir_) / "Exports").u8string();
    for (auto& v : lib_.videos) for (auto& l : v.lists) add_list(l);
    reindex();
    net_ = std::make_unique<HttpClient>(12);
    images_ = std::make_unique<ImageCache>(*net_);
    saver_ = std::make_unique<ThreadPool>(1);
    selftest_ = std::getenv("TUBEMASTER_SELFTEST") != nullptr;
    selftest_t0_ = now_sec();
    log("TubeMaster prêt — " + std::to_string(lib_.videos.size()) + " lien(s), " + std::to_string(lib_.lists.size()) +
        " liste(s).");
    refresh_tools();
    SDL_ShowWindow(win_);
    // Liens jamais vérifiés : vérification au démarrage ; dates de mise en ligne manquantes.
    std::vector<std::string> todo;
    for (auto& v : lib_.videos) {
        if (v.status == tub::Status::Unknown) todo.push_back(v.id);
        else if (v.status == tub::Status::Online && v.published.empty()) { pub_tried_[v.id] = true; pub_queue_.push_back(v.id); }
    }
    if (!todo.empty()) check_ids(todo);
}

App::~App() {
    if (dl_proc_) dl_proc_->kill();
    if (ch_proc_) ch_proc_->kill();
    save_now();
    saver_.reset();
    web_.reset();
    sweb_.reset();
    images_.reset();
    net_.reset();
}

void App::log(const std::string& msg) {
    log_.push_back("[" + fmt_local(std::time(nullptr), "%H:%M:%S") + "] " + msg);
    while (log_.size() > 300) log_.pop_front();
    diag("journal : " + msg);
}

// ── bibliothèque ─────────────────────────────────────────────────────────────
tub::Video* App::find(const std::string& id) {
    auto it = idx_.find(id);
    return it == idx_.end() ? nullptr : &lib_.videos[it->second];
}

void App::reindex() {
    idx_.clear();
    for (size_t i = 0; i < lib_.videos.size(); ++i) idx_[lib_.videos[i].id] = i;
    rows_dirty_ = true;
}

void App::add_list(const std::string& name) {
    if (name.empty() || name[0] == '\xC2') return;
    if (std::find(lib_.lists.begin(), lib_.lists.end(), name) == lib_.lists.end()) lib_.lists.push_back(name);
}

App::ImportStats App::import_text(const std::string& text, const std::string& list) {
    ImportStats st;
    auto links = yt::find_links(text);
    std::vector<std::string> fresh;
    time_t now = std::time(nullptr);
    if (!list.empty()) add_list(list);
    for (auto& l : links) {
        ++st.links;
        if (tub::Video* v = find(l.id)) {
            ++st.known;
            if (!list.empty() && std::find(v->lists.begin(), v->lists.end(), list) == v->lists.end()) v->lists.push_back(list);
            continue;
        }
        tub::Video v;
        v.id = l.id;
        v.start = l.start;
        v.added = now;
        v.order = lib_.next_order++;
        if (!list.empty()) v.lists.push_back(list);
        lib_.videos.push_back(std::move(v));
        idx_[l.id] = lib_.videos.size() - 1;
        fresh.push_back(l.id);
        ++st.added;
    }
    rows_dirty_ = true;
    if (lib_.cfg.check_after_import && !fresh.empty()) check_ids(fresh);
    save_soon();
    return st;
}

void App::import_files(const std::vector<std::string>& paths) {
    for (auto& p : paths) {
        std::string text = tub::read_file(p);
        std::string list = tub::list_name_from_path(p);
        ImportStats st = import_text(text, list);
        log("Import de « " + fs::u8path(p).filename().u8string() + " » : " + std::to_string(st.links) + " lien(s), " +
            std::to_string(st.added) + " nouveau(x), " + std::to_string(st.known) + " déjà présent(s) → liste « " + list + " ».");
        if (!st.links) log("Aucun lien YouTube trouvé dans ce fichier.");
        cur_list_ = list;
    }
    rows_dirty_ = true;
}

void App::paste_links() {
    char* c = SDL_GetClipboardText();
    std::string text = c ? c : "";
    SDL_free(c);
    std::string list = cur_list_.empty() || cur_list_[0] == '\xC2' ? std::string() : cur_list_;
    ImportStats st = import_text(text, list);
    log("Presse-papiers : " + std::to_string(st.links) + " lien(s), " + std::to_string(st.added) + " nouveau(x).");
}

void App::on_drop_file(const std::string& path) {
    std::error_code ec;
    if (fs::is_directory(fs::u8path(path), ec)) return;
    import_files({path});
}

void App::on_drop_text(const std::string& text) {
    ImportStats st = import_text(text, cur_list_.empty() || cur_list_[0] == '\xC2' ? std::string() : cur_list_);
    log("Glisser-déposer : " + std::to_string(st.added) + " nouveau(x) lien(s).");
}

void App::remove_videos(const std::vector<std::string>& ids) {
    std::unordered_map<std::string, bool> del;
    for (auto& id : ids) del[id] = true;
    lib_.videos.erase(std::remove_if(lib_.videos.begin(), lib_.videos.end(), [&](const tub::Video& v) { return del.count(v.id) != 0; }),
                      lib_.videos.end());
    for (auto& id : ids) sel_.erase(id);
    reindex();
    log("Supprimé(s) de la bibliothèque : " + std::to_string(ids.size()) + " lien(s).");
    save_soon();
}

void App::remove_from_list(const std::vector<std::string>& ids, const std::string& list) {
    for (auto& id : ids)
        if (tub::Video* v = find(id)) v->lists.erase(std::remove(v->lists.begin(), v->lists.end(), list), v->lists.end());
    rows_dirty_ = true;
    save_soon();
}

void App::add_to_list(const std::vector<std::string>& ids, const std::string& list) {
    add_list(list);
    for (auto& id : ids)
        if (tub::Video* v = find(id))
            if (std::find(v->lists.begin(), v->lists.end(), list) == v->lists.end()) v->lists.push_back(list);
    log(std::to_string(ids.size()) + " lien(s) ajouté(s) à la liste « " + list + " ».");
    rows_dirty_ = true;
    save_soon();
}

void App::export_list_txt() {
    std::string name = cur_list_.empty() ? "Tous les liens" : cur_list_[0] == '\xC2' ? "Selection" : cur_list_;
    std::string path = pick_save_file("Enregistrer la liste", name + ".txt", "Texte|*.txt");
    if (path.empty()) return;
    std::string out;
    for (size_t i : rows_) out += yt::watch_url(lib_.videos[i].id, lib_.videos[i].start) + "\r\n";
    if (tub::write_file_atomic(path, out)) log("Liste enregistrée : " + path + " (" + std::to_string(rows_.size()) + " liens).");
    else log("Impossible d'écrire " + path);
}

std::vector<std::string> App::selected_ids() const {
    std::vector<std::string> v;
    for (size_t i : rows_)
        if (sel_.count(lib_.videos[i].id)) v.push_back(lib_.videos[i].id);
    return v;
}

std::string App::display_title(const tub::Video& v) const {
    if (!v.title.empty()) return v.title;
    if (!v.arch_title.empty()) return "† " + v.arch_title;
    if (v.status == tub::Status::Removed || v.status == tub::Status::Private) return "(vidéo indisponible)";
    if (v.checking) return "…";
    return "(titre inconnu)";
}

void App::rebuild_rows() {
    rows_dirty_ = false;
    rows_.clear();
    std::string q = lower(search_);
    for (size_t i = 0; i < lib_.videos.size(); ++i) {
        const tub::Video& v = lib_.videos[i];
        bool dead = v.status == tub::Status::Removed || v.status == tub::Status::Private;
        if (cur_list_ == "\xC2\xA7" "dead" && !dead) continue;
        if (cur_list_ == "\xC2\xA7" "unknown" && !(v.status == tub::Status::Unknown || v.status == tub::Status::Error)) continue;
        if (cur_list_ == "\xC2\xA7" "done" && v.file.empty()) continue;
        if (!cur_list_.empty() && cur_list_[0] != '\xC2' &&
            std::find(v.lists.begin(), v.lists.end(), cur_list_) == v.lists.end())
            continue;
        if (!q.empty() && !icontains(v.title, q) && !icontains(v.channel, q) && !icontains(v.id, q) &&
            !icontains(v.arch_title, q) && !icontains(v.arch_channel, q))
            continue;
        rows_.push_back(i);
    }
    int key = lib_.cfg.sort;
    bool desc = lib_.cfg.sort_desc;
    auto& vids = lib_.videos;
    std::stable_sort(rows_.begin(), rows_.end(), [&](size_t a, size_t b) {
        const tub::Video& x = vids[a];
        const tub::Video& y = vids[b];
        int c = 0;
        switch (key) {
        case 1: c = lower(display_title(x)).compare(lower(display_title(y))); break;
        case 2: c = lower(x.channel.empty() ? x.arch_channel : x.channel).compare(lower(y.channel.empty() ? y.arch_channel : y.channel)); break;
        case 3: c = int(x.status) - int(y.status); break;
        case 4: c = x.added < y.added ? -1 : x.added > y.added ? 1 : 0; break;
        case 5: c = x.id.compare(y.id); break;
        case 6: {
            // Sans date : en fin de liste, quel que soit le sens.
            const std::string& dx = x.published.empty() ? x.arch_date : x.published;
            const std::string& dy = y.published.empty() ? y.arch_date : y.published;
            if (dx.empty() != dy.empty()) return dy.empty();
            c = dx.compare(dy);
            break;
        }
        default: c = x.order - y.order; break;
        }
        if (c == 0) c = x.order - y.order;
        return desc ? c > 0 : c < 0;
    });
}

void App::save_soon() {
    if (save_due_ == 0) save_due_ = now_sec() + 1.5;
}

void App::save_now() {
    save_due_ = 0;
    auto data = std::make_shared<std::string>(tub::serialize_library(lib_));
    std::string path = data_path_;
    if (saver_) saver_->submit([data, path] { tub::write_file_atomic(path, *data); });
}

// ── date de mise en ligne ────────────────────────────────────────────────────
void App::fetch_published(const std::string& id) {
    ++pub_inflight_;
    HttpRequest req;
    req.url = yt::watch_url(id);
    // SOCS : passe la page de consentement aux cookies (Europe) sans compte.
    req.headers = {"Accept-Language: en-US,en;q=0.9", "Cookie: SOCS=CAI; PREF=hl=en"};
    req.max_bytes = 3u << 20;
    req.timeout_ms = 20000;
    net_->get(std::move(req), [this, id](HttpResponse&& r) {
        yt::Archived a;
        if (r.ok()) yt::parse_watch_page(r.body, a);
        std::string d = yt::normalize_date(a.date);
        long code = r.status;
        size_t size = r.body.size();
        post_main([this, id, d, code, size] {
            --pub_inflight_;
            tub::Video* v = find(id);
            if (!v) return;
            if (d.size() < 10 || d[4] != '-') {
                // Page sans la date (vérification « robot », consentement…) : deux nouveaux essais espacés.
                int n = ++pub_attempts_[id];
                diag("date de mise en ligne " + id + " : absente (HTTP " + std::to_string(code) + ", " + std::to_string(size) +
                     " octets), essai " + std::to_string(n));
                if (n < 3) pub_later_.push_back({now_sec() + 20.0 * n, id});
                return;
            }
            v->published = d.substr(0, 10);
            rows_dirty_ = true;
            save_soon();
        });
    });
}

std::string App::display_date(const tub::Video& v) const {
    auto fr = [](const std::string& d) {
        return d.size() >= 10 && d[4] == '-' ? d.substr(8, 2) + "/" + d.substr(5, 2) + "/" + d.substr(0, 4) : d;
    };
    if (!v.published.empty()) return fr(v.published);
    if (!v.arch_date.empty()) return "† " + fr(v.arch_date);
    return {};
}

// ── miniatures conservées ────────────────────────────────────────────────────
std::string App::thumb_path(const std::string& id) const {
    return (fs::u8path(base_dir_) / "miniatures" / fs::u8path(id + ".jpg")).u8string();
}

void App::save_thumb(const std::string& id) {
    std::string path = thumb_path(id);
    if (file_exists(path) || thumb_saving_.count(id)) return;
    thumb_saving_[id] = true;
    HttpRequest req;
    req.url = yt::thumb_url(id);
    req.max_bytes = 2u << 20;
    net_->get(std::move(req), [this, id, path](HttpResponse&& r) {
        auto body = std::make_shared<std::string>(r.ok() && r.body.size() > 1000 ? std::move(r.body) : std::string());
        post_main([this, id, path, body] {
            thumb_saving_.erase(id);
            if (body->empty()) return;
            saver_->submit([path, body] {
                std::error_code ec;
                fs::create_directories(fs::u8path(path).parent_path(), ec);
                tub::write_file_atomic(path, *body);
            });
        });
    });
}

void App::move_to_channel_lists(const std::vector<std::string>& ids) {
    bool from_list = !cur_list_.empty() && cur_list_[0] != '\xC2';
    int moved = 0, skipped = 0;
    std::unordered_map<std::string, int> per;
    for (auto& id : ids) {
        tub::Video* v = find(id);
        if (!v) continue;
        std::string ch = trim(v->channel.empty() ? v->arch_channel : v->channel);
        if (ch.empty()) { ++skipped; continue; }
        add_list(ch);
        if (std::find(v->lists.begin(), v->lists.end(), ch) == v->lists.end()) v->lists.push_back(ch);
        if (from_list && cur_list_ != ch) v->lists.erase(std::remove(v->lists.begin(), v->lists.end(), cur_list_), v->lists.end());
        ++per[ch];
        ++moved;
    }
    log(std::to_string(moved) + " lien(s) rangé(s) dans " + std::to_string(per.size()) + " liste(s) de chaîne" +
        (skipped ? ", " + std::to_string(skipped) + " sans chaîne connue (non déplacé(s))" : std::string()) + ".");
    for (auto it = sel_.begin(); it != sel_.end();) it = find(it->first) ? std::next(it) : sel_.erase(it);
    rows_dirty_ = true;
    save_soon();
}

// ── vérification (oEmbed) ────────────────────────────────────────────────────
void App::check_ids(const std::vector<std::string>& ids) {
    if (check_queue_.empty() && check_inflight_ == 0) { check_total_ = 0; check_done_ = 0; }
    for (auto& id : ids) {
        if (tub::Video* v = find(id)) {
            if (v->checking) continue;
            v->checking = true;
            check_queue_.push_back(id);
            ++check_total_;
        }
    }
}

void App::check_one(const std::string& id) {
    ++check_inflight_;
    HttpRequest req;
    req.url = yt::oembed_url(id);
    req.headers = {"Accept: application/json", "Accept-Language: fr-FR,fr;q=0.9"};
    req.max_bytes = 64u << 10;
    net_->get(std::move(req), [this, id](HttpResponse&& r) {
        yt::OEmbed o;
        if (r.status == 200) yt::parse_oembed(r.body, o);
        long code = r.error.empty() ? r.status : 0;
        if (code == 401 || code == 403) {
            // Intégration désactivée ou vidéo privée : la miniature tranche (privée/supprimée = 404).
            HttpRequest t;
            t.url = "https://i.ytimg.com/vi/" + id + "/hqdefault.jpg";
            t.max_bytes = 4096;
            net_->get(std::move(t), [this, id, o](HttpResponse&& tr) {
                long c = tr.status == 200 ? 200 : 403;
                post_main([this, id, c, o] { apply_check(id, c, o); });
            });
            return;
        }
        post_main([this, id, code, o] { apply_check(id, code, o); });
    });
}

void App::apply_check(const std::string& id, long http, const yt::OEmbed& o) {
    --check_inflight_;
    ++check_done_;
    tub::Video* v = find(id);
    if (!v) return;
    v->checking = false;
    tub::Status before = v->status;
    if (http == 200) {
        v->status = tub::Status::Online;
        if (!o.title.empty()) v->title = o.title;
        if (!o.author.empty()) { v->channel = o.author; v->channel_url = o.author_url; }
        save_thumb(id);
        if (v->published.empty() && !pub_tried_.count(id)) { pub_tried_[id] = true; pub_queue_.push_back(id); }
    } else if (http == 401 || http == 403) {
        v->status = tub::Status::Private;
    } else if (http == 404 || http == 400) {
        v->status = tub::Status::Removed;
    } else {
        v->status = tub::Status::Error;
    }
    v->checked = std::time(nullptr);
    if (before == tub::Status::Online && (v->status == tub::Status::Removed || v->status == tub::Status::Private))
        log("Lien devenu mort : " + id + (v->title.empty() ? "" : " (" + v->title + ")"));
    if (check_done_ >= check_total_ && check_queue_.empty() && check_inflight_ == 0) {
        int dead = 0;
        for (auto& x : lib_.videos) if (x.status == tub::Status::Removed || x.status == tub::Status::Private) ++dead;
        log("Vérification terminée : " + std::to_string(check_total_) + " lien(s), " + std::to_string(dead) + " mort(s) dans la bibliothèque.");
    }
    rows_dirty_ = true;
    save_soon();
}

// ── import d'une chaîne ou d'une playlist (yt-dlp) ───────────────────────────
void App::start_channel_import() {
    if (ch_running_) { if (ch_proc_) ch_proc_->kill(); return; }
    std::string exe = tool("yt-dlp");
    if (exe.empty()) { log("yt-dlp introuvable : voir l'onglet Export (bouton « Télécharger yt-dlp »)."); return; }
    std::string in = trim(channel_input_);
    if (in.empty()) return;
    std::vector<std::string> urls;
    std::string name;
    if (in.find("list=") != std::string::npos) {
        urls.push_back(in);
        name = "Playlist " + in.substr(in.find("list=") + 5, 12);
    } else {
        std::string base = yt::channel_url(in);
        for (const char* tab : {"/videos", "/shorts", "/streams", "/featured", "/playlists"}) {
            size_t p = base.rfind(tab);
            if (p != std::string::npos && p + std::strlen(tab) == base.size()) base.resize(p);
        }
        while (!base.empty() && base.back() == '/') base.pop_back();
        urls.push_back(base + "/videos");
        if (lib_.cfg.include_shorts) urls.push_back(base + "/shorts");
        if (lib_.cfg.include_live) urls.push_back(base + "/streams");
        size_t s = base.find_last_of('/');
        name = "Chaîne " + (s == std::string::npos ? base : base.substr(s + 1));
    }
    std::vector<std::string> args = {"--flat-playlist", "--encoding", "utf-8", "--ignore-errors", "--print", "%(id)s\t%(title)s"};
    if (channel_limit_ > 0) { args.push_back("-I"); args.push_back("1:" + std::to_string(channel_limit_)); }
    if (!lib_.cfg.cookies.empty()) { args.push_back("--cookies"); args.push_back(lib_.cfg.cookies); }
    args.insert(args.end(), urls.begin(), urls.end());
    ch_list_ = name;
    ch_count_ = 0;
    add_list(name);
    cur_list_ = name;
    rows_dirty_ = true;
    std::string err;
    log("Import de « " + in + " » avec yt-dlp…");
    ch_proc_ = Process::start(
        exe, args, tools_dir(),
        [this](const std::string& line) {
            std::string id, title;
            if (yt::parse_channel_line(line, id, title)) {
                post_main([this, id, title] {
                    ImportStats st = import_text(yt::watch_url(id), ch_list_);
                    if (tub::Video* v = find(id)) if (v->title.empty() && !title.empty()) v->title = title;
                    ch_count_ += st.links;
                });
            } else if (line.find("ERROR") != std::string::npos) {
                post_main([this, line] { log("yt-dlp : " + line.substr(0, 200)); });
            }
        },
        [this](int code) {
            post_main([this, code] {
                ch_running_ = false;
                log("Import de chaîne terminé : " + std::to_string(ch_count_) + " vidéo(s) dans « " + ch_list_ + " »" +
                    (code ? " (yt-dlp a signalé des erreurs)" : "") + ".");
            });
        },
        &err);
    ch_running_ = ch_proc_ != nullptr;
    if (!ch_proc_) log("Lancement de yt-dlp impossible : " + err);
}

// ── lecteur ──────────────────────────────────────────────────────────────────
void App::ensure_webview() {
    if (web_ || web_tried_) return;
    web_tried_ = true;
    std::string why;
    web_ = WebView::create(win_, (fs::u8path(base_dir_) / "webview_data").u8string(), &why);
    if (!web_) { web_error_ = why; log("Lecteur intégré indisponible : " + why); return; }
    // Fin de la vidéo (hors publicité) : enchaînement sur la suivante.
    web_->add_startup_script(
        "document.addEventListener('ended',function(e){var v=e.target;if(!v||v.tagName!=='VIDEO')return;"
        "if(document.querySelector('.ad-showing'))return;if(v.duration&&v.duration<20)return;"
        "if(window.chrome&&window.chrome.webview)window.chrome.webview.postMessage('ended');},true);");
    web_->on_key = [this](WebKey k) {
        if (k == WebKey::Ended && autoplay_next_) play_step(+1);
        else if (k == WebKey::NextChannel) play_step(+1);
        else if (k == WebKey::PrevChannel) play_step(-1);
        else if (k == WebKey::Fullscreen) web_->set_detached_fullscreen(!web_->detached_fullscreen());
        else if (k == WebKey::Escape && web_->detached_fullscreen()) web_->set_detached_fullscreen(false);
    };
    web_->on_fullscreen = [this](bool fs) { if (web_) web_->set_detached_fullscreen(fs); };
}

void App::play(const std::string& id) {
    ensure_webview();
    play_id_ = id;
    // La liste de lecture : les liens affichés au moment du lancement.
    if (std::find(playlist_.begin(), playlist_.end(), id) == playlist_.end()) {
        playlist_.clear();
        for (size_t i : rows_) playlist_.push_back(lib_.videos[i].id);
        if (std::find(playlist_.begin(), playlist_.end(), id) == playlist_.end()) playlist_.push_back(id);
    }
    set_view(View::Player);
    if (!web_) return;
    tub::Video* v = find(id);
    web_->navigate(yt::watch_url(id, v ? v->start : 0));
    web_->focus();
}

void App::play_step(int delta) {
    if (playlist_.empty()) return;
    auto it = std::find(playlist_.begin(), playlist_.end(), play_id_);
    int i = it == playlist_.end() ? -1 : int(it - playlist_.begin());
    int n = int(playlist_.size());
    int next = ((i + delta) % n + n) % n;
    play(playlist_[size_t(next)]);
}

void App::set_view(View v) {
    if (v != View::Player && web_ && web_->detached_fullscreen()) web_->set_detached_fullscreen(false);
    if (view_ == View::Player && v != View::Player && web_) web_->run_script("document.querySelectorAll('video').forEach(function(x){x.pause();});");
    view_ = v;
}

// ── outils (yt-dlp, ffmpeg, deno) ────────────────────────────────────────────
std::string App::tools_dir() const {
    return lib_.cfg.tools_dir.empty() ? base_dir_ : lib_.cfg.tools_dir;
}

std::string App::tool(const char* name) const {
    std::string file = std::string(name) + kExe;
    for (const std::string& d : {lib_.cfg.tools_dir, base_dir_}) {
        if (d.empty()) continue;
        std::string p = (fs::u8path(d) / fs::u8path(file)).u8string();
        if (file_exists(p)) return p;
    }
    const char* path = std::getenv("PATH");
    std::string ps = path ? path : "";
    size_t a = 0;
    while (a <= ps.size()) {
        size_t e = ps.find(kPathSep, a);
        std::string d = ps.substr(a, e == std::string::npos ? std::string::npos : e - a);
        if (!d.empty()) {
            std::string p = (fs::u8path(d) / fs::u8path(file)).u8string();
            if (file_exists(p)) return p;
        }
        if (e == std::string::npos) break;
        a = e + 1;
    }
    return {};
}

void App::refresh_tools() {
    std::string exe = tool("yt-dlp");
    if (exe.empty()) { ytdlp_version_.clear(); return; }
    auto ver = std::make_shared<std::string>();
    std::string err;
    Process::start(exe, {"--version"}, tools_dir(),
                   [ver](const std::string& l) { if (ver->empty()) *ver = l; },
                   [this, ver](int) { post_main([this, ver] { ytdlp_version_ = *ver; }); }, &err);
}

void App::download_tool(const std::string& name) {
    if (tool_busy_.count(name)) return;
    std::string url;
    bool zip = false;
#ifdef _WIN32
    if (name == "yt-dlp") url = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
    else if (name == "ffmpeg") { url = "https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"; zip = true; }
    else if (name == "deno") { url = "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip"; zip = true; }
#else
    if (name == "yt-dlp") url = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp";
#endif
    if (url.empty()) { log("Téléchargement automatique de " + name + " non disponible sur ce système."); return; }
    std::string dir = tools_dir();
    tool_busy_[name] = "téléchargement…";
    log("Téléchargement de " + name + " : " + url);
    HttpRequest req;
    req.url = url;
    req.max_bytes = size_t(400) << 20;
    req.timeout_ms = 20 * 60 * 1000;
    req.urgent = true;
    net_->get(std::move(req), [this, name, dir, zip](HttpResponse&& r) {
        bool ok = r.ok() && r.body.size() > 100000;
        std::string err = ok ? std::string() : (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error);
        auto body = std::make_shared<std::string>(std::move(r.body));
        post_main([this, name, dir, zip, ok, err, body] {
            if (!ok) { tool_busy_.erase(name); log("Échec du téléchargement de " + name + " : " + err); return; }
            std::error_code ec;
            fs::create_directories(fs::u8path(dir), ec);
            std::string target = (fs::u8path(dir) / fs::u8path(zip ? name + ".zip" : name + kExe)).u8string();
            if (!tub::write_file_atomic(target, *body)) { tool_busy_.erase(name); log("Écriture impossible : " + target); return; }
#ifndef _WIN32
            fs::permissions(fs::u8path(target), fs::perms::owner_exec | fs::perms::group_exec, fs::perm_options::add, ec);
#endif
            if (!zip) { tool_busy_.erase(name); log(name + " installé : " + target); refresh_tools(); return; }
            // Archive : extraction de l'exécutable avec l'outil « tar » de Windows 10/11.
            tool_busy_[name] = "extraction…";
            std::vector<std::string> args = {"-xf", target, "-C", dir};
            if (name == "ffmpeg") {
                args.insert(args.end(), {"--strip-components", "2", "ffmpeg-master-latest-win64-gpl/bin/ffmpeg.exe",
                                         "ffmpeg-master-latest-win64-gpl/bin/ffprobe.exe"});
            }
            std::string e2;
            tool_proc_ = Process::start("tar", args, std::string(), [this](const std::string& l) { post_main([this, l] { log("tar : " + l); }); },
                                        [this, name, target](int code) {
                                            post_main([this, name, target, code] {
                                                std::error_code ec2;
                                                fs::remove(fs::u8path(target), ec2);
                                                tool_busy_.erase(name);
                                                std::string p = tool(name.c_str());
                                                log(p.empty() ? name + " : extraction échouée (code " + std::to_string(code) + ")."
                                                              : name + " installé : " + p);
                                            });
                                        },
                                        &e2);
            if (!tool_proc_) { tool_busy_.erase(name); log("Extraction impossible : " + e2); }
        });
    });
}

void App::update_ytdlp() {
    std::string exe = tool("yt-dlp");
    if (exe.empty()) { download_tool("yt-dlp"); return; }
    if (dl_running_) { log("Mise à jour impossible pendant les téléchargements."); return; }
    std::string err;
    console_.push_back(">>> yt-dlp -U");
    tool_proc_ = Process::start(exe, {"-U"}, tools_dir(),
                                [this](const std::string& l) { post_main([this, l] { console_.push_back(l); }); },
                                [this](int) { post_main([this] { console_.push_back(">>> mise à jour terminée"); updating_ytdlp_ = false; refresh_tools(); }); },
                                &err);
    if (!tool_proc_) log("yt-dlp : " + err);
    else updating_ytdlp_ = true;
}

// ── export ───────────────────────────────────────────────────────────────────
void App::enqueue(const std::vector<std::string>& ids) {
    int n = 0;
    for (auto& id : ids) {
        tub::Video* v = find(id);
        enqueue_url(yt::watch_url(id), v ? display_title(*v) : id);
        ++n;
    }
    log(std::to_string(n) + " lien(s) ajouté(s) à la file d'export (" + yt::format_name(yt::Format(lib_.cfg.format)) + ", " +
        yt::quality_name(yt::Quality(lib_.cfg.quality), yt::Format(lib_.cfg.format)) + ").");
}

void App::enqueue_url(const std::string& url, const std::string& title) {
    DlJob j;
    j.url = url;
    j.id = yt::extract_id(url);
    j.title = title.empty() ? url : title;
    j.format = yt::Format(lib_.cfg.format);
    j.quality = yt::Quality(lib_.cfg.quality);
    jobs_.push_back(std::move(j));
}

std::string App::job_dir(const DlJob& j) const {
    fs::path d = fs::u8path(lib_.cfg.dest_dir);
    if (lib_.cfg.subfolders) {
        std::string sub = std::string(yt::format_name(j.format)) + "-" +
                          (j.format == yt::Format::MP3 ? (j.quality == yt::Quality::Max ? "max" : "192k")
                                                       : (j.quality == yt::Quality::Max ? "max" : "720p"));
        d /= fs::u8path(sub);
    }
    return d.u8string();
}

// yt-dlp de plus de 30 jours : YouTube change souvent, une version ancienne reste bloquée à 0 %.
bool App::ytdlp_outdated() const {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(ytdlp_version_.c_str(), "%d.%d.%d", &y, &m, &d) != 3) return false;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 12;
    time_t v = std::mktime(&tm);
    return v > 0 && std::time(nullptr) - v > 30 * 86400;
}

void App::start_downloads() {
    if (dl_running_ || start_pending_) return;
    // Outils manquants (deno compris : sans lui, YouTube refuse les morceaux de vidéo et
    // l'export reste à 0 %) : installation, puis départ automatique.
    bool wait = false;
    for (const char* n : {"yt-dlp", "ffmpeg", "deno"}) {
        if (tool(n).empty()) {
            console_.push_back(std::string("Installation de ") + n + "…");
            download_tool(n);
            wait = true;
        }
    }
    if (!wait && !update_checked_ && ytdlp_outdated()) {
        update_checked_ = true;
        console_.push_back("yt-dlp " + ytdlp_version_ + " a plus de 30 jours : mise à jour avant l'export…");
        update_ytdlp();
        wait = true;
    }
    if (wait) { start_pending_ = true; return; }
    if (tool("yt-dlp").empty()) { log("yt-dlp introuvable : cliquez sur « Télécharger yt-dlp »."); return; }
    if (tool("ffmpeg").empty()) log("Attention : ffmpeg introuvable — l'assemblage MKV/MP4 et la conversion MP3 échoueront.");
    dl_running_ = true;
    next_pause_at_ = lib_.cfg.pause_enabled ? now_sec() + std::max(1, lib_.cfg.pause_every_min) * 60.0 : 0;
    pause_until_ = 0;
    save_soon();
    next_download();
}

void App::stop_downloads() {
    dl_running_ = false;
    pause_until_ = 0;
    if (dl_proc_) { dl_proc_->kill(); console_.push_back("⛔ Arrêt demandé."); }
}

void App::next_download() {
    if (!dl_running_ || dl_proc_) return;
    if (next_pause_at_ > 0 && now_sec() >= next_pause_at_) {
        pause_until_ = now_sec() + std::max(1, lib_.cfg.pause_sec);
        next_pause_at_ = 0;
        console_.push_back("⏸ Pause automatique de " + std::to_string(lib_.cfg.pause_sec) + " s.");
        return;
    }
    int idx = -1;
    for (size_t i = 0; i < jobs_.size(); ++i) if (jobs_[i].state == DlJob::Waiting) { idx = int(i); break; }
    if (idx < 0) {
        dl_running_ = false;
        int ok = 0, ko = 0;
        for (auto& j : jobs_) { if (j.state == DlJob::Done) ++ok; if (j.state == DlJob::Failed) ++ko; }
        log("Export terminé : " + std::to_string(ok) + " réussi(s), " + std::to_string(ko) + " en erreur.");
        console_.push_back("==== Terminé : " + std::to_string(ok) + " réussi(s), " + std::to_string(ko) + " en erreur ====");
        if (lib_.cfg.remove_done)
            jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const DlJob& j) { return j.state == DlJob::Done; }), jobs_.end());
        return;
    }
    DlJob& j = jobs_[size_t(idx)];
    dl_cur_ = idx;
    j.state = DlJob::Running;
    j.pct = 0;
    j.error.clear();
    j.status.clear();
    j.started = now_sec();
    j.stall_warned = false;
    yt::Job spec;
    spec.url = j.url;
    spec.format = j.format;
    spec.quality = j.quality;
    spec.dest_dir = job_dir(j);
    spec.cookies = lib_.cfg.cookies;
    std::string ff = tool("ffmpeg");
    if (!ff.empty()) spec.ffmpeg_dir = fs::u8path(ff).parent_path().u8string();
    // Moteur JavaScript pour YouTube : chemin donné explicitement (option des versions ≥ 2025.11).
    std::string deno = tool("deno");
    if (!deno.empty() && ytdlp_version_ >= "2025.11.12") spec.js_runtime = "deno:" + deno;
    std::error_code ec;
    fs::create_directories(fs::u8path(spec.dest_dir), ec);
    auto args = yt::download_args(spec);
    std::string exe = tool("yt-dlp");
    console_.push_back("-------- " + j.title);
    console_.push_back(command_line("yt-dlp", args));
    diag("export : " + command_line("yt-dlp", args));
    std::string err;
    int my = idx;
    dl_proc_ = Process::start(
        exe, args, tools_dir(),
        [this, my](const std::string& line) {
            post_main([this, my, line] {
                if (my >= int(jobs_.size())) return;
                DlJob& jj = jobs_[size_t(my)];
                yt::Progress p;
                if (line.rfind(yt::kFileMark, 0) == 0) { jj.file = line.substr(std::strlen(yt::kFileMark)); return; }
                if (yt::parse_progress(line, p)) {
                    jj.pct = p.pct;
                    jj.speed = p.speed;
                    jj.eta = p.eta;
                    if (!console_.empty() && console_.back().rfind("[download]", 0) == 0 && line.rfind("[download]", 0) == 0)
                        console_.back() = line;            // une seule ligne de progression
                    else
                        console_.push_back(line);
                } else {
                    console_.push_back(line);
                    if (line.find("ERROR") != std::string::npos) jj.error = line.substr(0, 300);
                    if (!line.empty() && (line[0] == '[' || line.rfind("WARNING", 0) == 0 || line.rfind("ERROR", 0) == 0))
                        jj.status = line.substr(0, 140);
                }
                while (console_.size() > 600) console_.pop_front();
            });
        },
        [this, my](int code) {
            post_main([this, my, code] {
                dl_proc_.reset();
                if (my < int(jobs_.size())) {
                    DlJob& jj = jobs_[size_t(my)];
                    if (!dl_running_) jj.state = DlJob::Stopped;
                    else if (code == 0) {
                        jj.state = DlJob::Done;
                        jj.pct = 100;
                        if (tub::Video* v = find(jj.id)) { v->file = jj.file; rows_dirty_ = true; save_soon(); }
                        console_.push_back("✅ Terminé : " + (jj.file.empty() ? jj.title : jj.file));
                    } else {
                        jj.state = DlJob::Failed;
                        if (jj.error.empty()) jj.error = "code de sortie " + std::to_string(code);
                        console_.push_back("⚠ Échec : " + jj.title + " — " + jj.error);
                        if (jj.error.find("Sign in to confirm") != std::string::npos)
                            console_.push_back("  → YouTube demande une connexion : choisissez votre fichier cookies.txt.");
                        if (jj.error.find("ffmpeg not found") != std::string::npos ||
                            jj.error.find("ffprobe and ffmpeg not found") != std::string::npos)
                            console_.push_back("  → ffmpeg manquant : bouton « Télécharger ffmpeg ».");
                        else if (jj.error.find("audio codec") != std::string::npos)
                            console_.push_back("  → pas de piste audio dans cette vidéo.");
                    }
                }
                dl_cur_ = -1;
                next_download();
            });
        },
        &err);
    if (!dl_proc_) {
        j.state = DlJob::Failed;
        j.error = "yt-dlp ne se lance pas : " + err;
        console_.push_back(j.error);
        dl_running_ = false;
    }
}

// ── lien mort ────────────────────────────────────────────────────────────────
void App::dead_search(const std::string& input, bool show) {
    std::string id = yt::extract_id(input);
    if (id.empty()) { log("Lien ou identifiant YouTube non reconnu : " + input); return; }
    dead_ = std::make_unique<DeadSearch>();
    dead_->id = id;
    dead_input_ = yt::watch_url(id);
    if (show) set_view(View::Dead);
    log("Recherche d'origine de " + id + "…");
    // 1) YouTube : la vidéo existe-t-elle encore ?
    HttpRequest a;
    a.url = yt::oembed_url(id);
    a.urgent = true;
    net_->get(std::move(a), [this, id](HttpResponse&& r) {
        yt::OEmbed o;
        if (r.status == 200) yt::parse_oembed(r.body, o);
        long code = r.error.empty() ? r.status : 0;
        post_main([this, id, code, o] {
            if (!dead_ || dead_->id != id) return;
            dead_->yt_status = int(code);
            dead_->oembed = o;
        });
    });
    dead_fetch_cdx();
}

// Wayback Machine : captures de la page de la vidéo (nouvel essai si le service est surchargé).
void App::dead_fetch_cdx() {
    if (!dead_) return;
    std::string id = dead_->id;
    ++dead_->cdx_tries;
    dead_->cdx_retry_at = 0;
    HttpRequest c;
    c.url = yt::wayback_cdx_url(id);
    c.urgent = true;
    c.timeout_ms = 40000;
    c.max_bytes = 2u << 20;
    net_->get(std::move(c), [this, id](HttpResponse&& r) {
        std::vector<std::string> snaps = r.ok() ? yt::parse_cdx(r.body) : std::vector<std::string>();
        std::string err = r.ok() ? std::string() : (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error);
        bool retry = !r.ok() && (r.status == 503 || r.status == 429 || r.status == 502 || r.status == 504 || r.status == 0);
        post_main([this, id, snaps, err, retry] {
            if (!dead_ || dead_->id != id) return;
            if (retry && dead_->cdx_tries < 4) {
                dead_->cdx_error = err + ", nouvel essai…";
                dead_->cdx_retry_at = now_sec() + 4.0 * dead_->cdx_tries;
                return;
            }
            dead_->cdx_done = true;
            dead_->cdx_error = err;
            dead_->snaps = snaps;
            if (snaps.empty()) { dead_->page_done = true; dead_store_result(); return; }
            dead_fetch_page();
        });
    });
}

// Page archivée : de la plus ancienne à la plus récente (les dernières montrent souvent
// « vidéo indisponible »), quatre essais au plus.
void App::dead_fetch_page() {
    if (!dead_ || dead_->snaps.empty()) return;
    const auto& s = dead_->snaps;
    std::vector<size_t> order = {0, s.size() / 3, s.size() / 2, s.size() - 1};
    if (dead_->page_tries >= int(order.size())) { dead_->page_done = true; dead_store_result(); return; }
    // Captures déjà lues : on passe à la suivante.
    while (dead_->page_tries < int(order.size()) && dead_->page_tries > 0 &&
           order[size_t(dead_->page_tries)] == order[size_t(dead_->page_tries - 1)])
        ++dead_->page_tries;
    if (dead_->page_tries >= int(order.size())) { dead_->page_done = true; dead_store_result(); return; }
    std::string ts = s[std::min(order[size_t(dead_->page_tries)], s.size() - 1)];
    ++dead_->page_tries;
    std::string id = dead_->id;
    HttpRequest p;
    p.url = yt::wayback_raw_url(ts, id);
    p.urgent = true;
    p.timeout_ms = 45000;
    p.max_bytes = 4u << 20;
    p.headers = {"Accept-Language: fr-FR,fr;q=0.9,en;q=0.8"};
    net_->get(std::move(p), [this, id, ts](HttpResponse&& r) {
        yt::Archived a;
        bool ok = r.ok() && yt::parse_watch_page(r.body, a);
        long code = r.status;
        post_main([this, id, ts, ok, a, code] {
            if (!dead_ || dead_->id != id) return;
            if (ok) {
                // Plusieurs captures se complètent (titre d'une époque, chaîne d'une autre…).
                yt::Archived& f = dead_->found;
                if (f.title.empty()) { f.title = a.title; dead_->found_ts = ts; }
                if (f.channel.empty()) f.channel = a.channel;
                if (f.channel_url.empty()) f.channel_url = a.channel_url;
                if (f.date.empty()) f.date = a.date;
                if (f.views.empty()) f.views = a.views;
                if (f.description.empty()) f.description = a.description;
                if (!f.channel.empty() && !f.date.empty()) {
                    dead_->page_done = true;
                    dead_store_result();
                    return;
                }
                dead_fetch_page();
                return;
            }
            diag("capture " + ts + " de " + id + " : HTTP " + std::to_string(code) + ", pas de titre");
            if ((code == 429 || code == 503) && dead_->page_waits < 4) {
                // Les archives limitent le rythme : même capture, quelques secondes plus tard.
                ++dead_->page_waits;
                --dead_->page_tries;
                dead_->page_retry_at = now_sec() + 5.0 * dead_->page_waits;
                return;
            }
            dead_fetch_page();
        });
    });
}

void App::dead_store_result() {
    if (!dead_) return;
    const DeadSearch& d = *dead_;
    if (!d.found.title.empty()) log("Retrouvé dans les archives : « " + d.found.title + " »" + (d.found.channel.empty() ? "" : " — " + d.found.channel));
    else log("Aucune information retrouvée automatiquement pour " + d.id + " (" + std::to_string(d.snaps.size()) + " capture(s)).");
    if (tub::Video* v = find(d.id)) {
        v->arch_snaps = int(d.snaps.size());
        v->arch_first = d.snaps.empty() ? std::string() : d.snaps.front();
        v->arch_last = d.snaps.empty() ? std::string() : d.snaps.back();
        if (!d.found.title.empty()) {
            v->arch_title = d.found.title;
            v->arch_channel = d.found.channel;
            v->arch_channel_url = d.found.channel_url;
            std::string iso = yt::normalize_date(d.found.date);
            v->arch_date = iso.empty() ? d.found.date : iso;     // AAAA-MM-JJ si possible (tri par date)
        }
        rows_dirty_ = true;
        save_soon();
    }
}

void App::analyze_all_dead() {
    arch_queue_.clear();
    for (auto& v : lib_.videos)
        if ((v.status == tub::Status::Removed || v.status == tub::Status::Private) && v.arch_snaps < 0) arch_queue_.push_back(v.id);
    arch_total_ = int(arch_queue_.size());
    arch_next_ = 0;
    log(arch_total_ ? "Analyse des archives pour " + std::to_string(arch_total_) + " lien(s) mort(s)…"
                    : "Tous les liens morts ont déjà été analysés.");
}

void App::ensure_search_view() {
    if (sweb_ || sweb_tried_) return;
    sweb_tried_ = true;
    std::string why;
    sweb_ = WebView::create(win_, (fs::u8path(base_dir_) / "webview_data").u8string(), &why);
    if (!sweb_) log("Navigateur intégré indisponible : " + why);
    else sweb_->on_source = [this](const std::string& u) { sweb_url_ = u; };
}

void App::browse(const std::string& url) {
    ensure_search_view();
    sweb_url_ = url;
    if (sweb_) sweb_->navigate(url);
}

// ── boucle ───────────────────────────────────────────────────────────────────
void App::update() {
    double now = now_sec();
    while (!pub_later_.empty() && now >= pub_later_.front().first) {
        pub_queue_.push_back(pub_later_.front().second);
        pub_later_.pop_front();
    }
    while (pub_inflight_ < 2 && !pub_queue_.empty()) {
        std::string id = pub_queue_.front();
        pub_queue_.pop_front();
        fetch_published(id);
    }
    while (check_inflight_ < 6 && !check_queue_.empty()) {
        std::string id = check_queue_.front();
        check_queue_.pop_front();
        check_one(id);
    }
    // Export demandé pendant l'installation / la mise à jour des outils : départ dès que c'est prêt.
    if (start_pending_ && tool_busy_.empty() && !updating_ytdlp_ && !(tool_proc_ && tool_proc_->running())) {
        start_pending_ = false;
        update_checked_ = true;
        if (tool("yt-dlp").empty()) console_.push_back("⚠ yt-dlp n'a pas pu être installé : voir le journal.");
        else start_downloads();
    }
    // Export bloqué à 0 % depuis une minute : causes probables dans la console.
    if (dl_running_ && dl_cur_ >= 0 && dl_cur_ < int(jobs_.size())) {
        DlJob& j = jobs_[size_t(dl_cur_)];
        if (j.state == DlJob::Running && j.pct <= 0 && !j.stall_warned && now - j.started > 60) {
            j.stall_warned = true;
            console_.push_back("⚠ Toujours à 0 % après une minute. Dernière étape : " + (j.status.empty() ? std::string("(rien)") : j.status));
            if (tool("deno").empty()) console_.push_back("  → deno manquant : YouTube refuse les morceaux de vidéo sans lui (bouton « Télécharger »).");
            console_.push_back("  → essayez « Mettre à jour » (yt-dlp " + ytdlp_version_ + "), puis relancez ; avec un fichier cookies récent.");
            console_.push_back("  → si ça persiste : « Copier la console » et envoyez-la.");
        }
    }
    // Pause automatique des exports.
    if (pause_until_ > 0 && now >= pause_until_) {
        pause_until_ = 0;
        next_pause_at_ = lib_.cfg.pause_enabled ? now + std::max(1, lib_.cfg.pause_every_min) * 60.0 : 0;
        console_.push_back("▶ Reprise.");
        next_download();
    }
    // Analyse des liens morts, un à la fois et sans hâte (les archives limitent le débit).
    if (!arch_queue_.empty() && now >= arch_next_ && (!dead_ || dead_->page_done)) {
        std::string id = arch_queue_.front();
        arch_queue_.pop_front();
        arch_next_ = now + 2.5;
        dead_search(id, false);
    }
    if (dead_ && dead_->cdx_retry_at > 0 && now >= dead_->cdx_retry_at) dead_fetch_cdx();
    if (dead_ && dead_->page_retry_at > 0 && now >= dead_->page_retry_at) { dead_->page_retry_at = 0; dead_fetch_page(); }
    if (web_) web_->set_visible(view_ == View::Player);
    if (sweb_) sweb_->set_visible(view_ == View::Dead);
    if (save_due_ > 0 && now >= save_due_) save_now();
    if (now >= next_glyph_scan_) { next_glyph_scan_ = now + 2; scan_glyphs(); }
    if (rows_dirty_) rebuild_rows();
    images_->gc();
    if (selftest_) selftest_step();
}

bool App::animating() const {
    return start_pending_ || pub_inflight_ > 0 || !pub_queue_.empty() || check_inflight_ > 0 || !check_queue_.empty() || dl_running_ || ch_running_ || !tool_busy_.empty() ||
           (dead_ && !dead_->page_done) || !arch_queue_.empty() || (web_ && !web_->ready() && !web_->failed());
}

int App::wait_timeout_ms() const { return selftest_ ? 100 : 500; }

} // namespace sw
