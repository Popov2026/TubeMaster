// TubeMaster — état de l'application et interface (Dear ImGui).
// Onglets : Liens (bibliothèque triée), Lecteur (YouTube intégré), Export (yt-dlp),
// Lien mort (origine d'une vidéo supprimée : Wayback Machine, Filmot…).
#pragma once
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "images.hpp"
#include "library.hpp"
#include "net.hpp"
#include "proc.hpp"
#include "util.hpp"
#include "webview.hpp"
#include "yt.hpp"

struct SDL_Window;
struct ImFont;

namespace sw {

// Caractères toujours chargés (latin, grec, cyrillique, ponctuation, flèches, formes, symboles).
extern const unsigned int kBaseGlyphRanges[];

struct Fonts {
    ImFont* main = nullptr;
    ImFont* small = nullptr;
    ImFont* title = nullptr;
    ImFont* big = nullptr;
};

enum class View { Links, Player, Export, Dead };

// Un téléchargement de la file d'export.
struct DlJob {
    enum State { Waiting, Running, Done, Failed, Stopped };
    std::string url, id, title;
    yt::Format format = yt::Format::MKV;
    yt::Quality quality = yt::Quality::Max;
    State state = Waiting;
    double pct = 0;
    std::string speed, eta, file, error;
    std::string status;                 // dernière ligne utile de yt-dlp (extraction, nouvel essai…)
    double started = 0;
    bool stall_warned = false;
};

// Recherche d'origine d'un lien mort.
struct DeadSearch {
    std::string id;
    int yt_status = -1;                 // code HTTP oEmbed (-1 : en cours)
    yt::OEmbed oembed;
    bool cdx_done = false, page_done = false;
    std::string cdx_error;
    std::vector<std::string> snaps;     // horodatages des captures
    yt::Archived found;
    std::string found_ts;               // capture d'où viennent les infos
    int page_tries = 0;
    int cdx_tries = 0;
    double cdx_retry_at = 0;                  // Wayback surchargée (503/429) : nouvel essai
    double page_retry_at = 0;                 // capture refusée (trop de demandes) : on patiente
    int page_waits = 0;
};

class App {
public:
    App(SDL_Window* win, const Fonts& fonts, float ui_scale);
    ~App();

    void update();                       // logique, une fois par tour de boucle
    void draw();                         // interface ImGui
    bool animating() const;
    int wait_timeout_ms() const;
    void request_quit() { quit_ = true; }
    bool want_quit() const { return quit_; }
    void on_drop_file(const std::string& path);
    void on_drop_text(const std::string& text);
    // Caractères des titres absents des polices chargées : demande de reconstruction de l'atlas.
    bool take_font_request(std::vector<unsigned>& extra);
    void set_fonts(const Fonts& f) { fonts_ = f; }

private:
    // ── bibliothèque ───────────────────────────────────────────────────────
    tub::Video* find(const std::string& id);
    void reindex();
    void add_list(const std::string& name);
    struct ImportStats { int links = 0, added = 0, known = 0; };
    ImportStats import_text(const std::string& text, const std::string& list);
    void import_files(const std::vector<std::string>& paths);
    void paste_links();
    void remove_videos(const std::vector<std::string>& ids);
    void remove_from_list(const std::vector<std::string>& ids, const std::string& list);
    void add_to_list(const std::vector<std::string>& ids, const std::string& list);
    void export_list_txt();
    // Miniature conservée sur le disque (miniatures/ID.jpg) dès que la vidéo est vue en ligne.
    std::string thumb_path(const std::string& id) const;
    void save_thumb(const std::string& id);
    // Chaque lien va dans la liste au nom de sa chaîne (et quitte la liste affichée).
    void move_to_channel_lists(const std::vector<std::string>& ids);
    std::vector<std::string> selected_ids() const;
    void rebuild_rows();
    std::string display_title(const tub::Video& v) const;
    std::string display_date(const tub::Video& v) const;   // « 23/04/2005 », « † 23/04/2005 » (archives)
    void save_soon();
    void save_now();
    void log(const std::string& msg);

    // vérification des liens (oEmbed)
    void check_ids(const std::vector<std::string>& ids);
    void check_one(const std::string& id);
    void apply_check(const std::string& id, long http, const yt::OEmbed& o);
    // Date de mise en ligne : page de la vidéo (datePublished), une fois par vidéo.
    void fetch_published(const std::string& id);
    std::deque<std::string> pub_queue_;
    int pub_inflight_ = 0;
    std::unordered_map<std::string, bool> pub_tried_;
    std::unordered_map<std::string, int> pub_attempts_;
    std::deque<std::pair<double, std::string>> pub_later_;   // nouvel essai différé (page refusée)

    // import d'une chaîne / playlist (yt-dlp)
    void start_channel_import();

    // ── lecteur ─────────────────────────────────────────────────────────────
    void ensure_webview();
    void play(const std::string& id);
    void play_step(int delta);

    // ── export (yt-dlp) ─────────────────────────────────────────────────────
    std::string tool(const char* name) const;       // chemin trouvé, ou vide
    std::string tools_dir() const;
    void refresh_tools();
    void download_tool(const std::string& name);
    void update_ytdlp();
    void enqueue(const std::vector<std::string>& ids);
    void enqueue_url(const std::string& url, const std::string& title);
    void start_downloads();
    void stop_downloads();
    void next_download();
    std::string job_dir(const DlJob& j) const;

    // ── lien mort ───────────────────────────────────────────────────────────
    void dead_search(const std::string& input, bool show = true);
    void dead_fetch_page();
    void dead_fetch_cdx();
    void dead_store_result();
    void ensure_search_view();
    void browse(const std::string& url);          // navigateur intégré (onglet Lien mort)
    void analyze_all_dead();

    // ── interface ───────────────────────────────────────────────────────────
    void apply_style();
    void draw_topbar();
    void draw_modals();
    void draw_links();
    void draw_links_side();
    void draw_links_table();
    void draw_player();
    void draw_export();
    void draw_dead();
    void draw_log_lines(float height);
    void set_view(View v);
    void selftest_step();

    SDL_Window* win_;
    Fonts fonts_;
    float scale_;
    bool quit_ = false;
    bool style_done_ = false;
    std::vector<unsigned> glyphs_;        // caractères supplémentaires (triés)
    bool glyphs_dirty_ = false;
    double next_glyph_scan_ = 0;
    void scan_glyphs();
    std::string base_dir_, data_path_;

    tub::Library lib_;
    std::unordered_map<std::string, size_t> idx_;
    std::unique_ptr<HttpClient> net_;
    std::unique_ptr<ImageCache> images_;
    std::unique_ptr<ThreadPool> saver_;
    double save_due_ = 0;
    std::deque<std::string> log_;
    std::unordered_map<std::string, bool> thumb_saving_;

    View view_ = View::Links;
    // Liens
    std::string cur_list_;              // "" tous, « §dead », « §unknown », « §done », sinon nom de liste
    std::string search_;
    std::vector<size_t> rows_;
    bool rows_dirty_ = true;
    std::unordered_map<std::string, bool> sel_;
    std::string anchor_;
    std::string add_input_, channel_input_, new_list_input_;
    int channel_limit_ = 0;             // 0 : tout
    std::shared_ptr<Process> ch_proc_;
    std::string ch_list_;
    int ch_count_ = 0;
    bool ch_running_ = false;
    std::deque<std::string> check_queue_;
    int check_inflight_ = 0, check_total_ = 0, check_done_ = 0;
    std::string confirm_delete_;        // « sel » ou nom de liste

    // Lecteur
    std::unique_ptr<WebView> web_;
    bool web_tried_ = false;
    std::string web_error_;
    std::string play_id_;
    std::vector<std::string> playlist_;
    bool autoplay_next_ = true;

    // Export
    std::vector<DlJob> jobs_;
    std::shared_ptr<Process> dl_proc_;
    bool dl_running_ = false;
    int dl_cur_ = -1;
    double next_pause_at_ = 0, pause_until_ = 0;
    std::deque<std::string> console_;
    std::string ytdlp_version_;
    std::unordered_map<std::string, std::string> tool_busy_;   // outil en cours de téléchargement → état
    std::string export_url_input_;
    std::shared_ptr<Process> tool_proc_;
    bool start_pending_ = false;        // export demandé : outils en cours d'installation / mise à jour
    bool updating_ytdlp_ = false;
    bool update_checked_ = false;       // yt-dlp vérifié (âge) pour cette session
    bool ytdlp_outdated() const;

    // Lien mort
    std::string dead_input_;
    std::unique_ptr<DeadSearch> dead_;
    std::unique_ptr<WebView> sweb_;     // navigateur de recherche
    bool sweb_tried_ = false;
    std::string sweb_url_;
    std::deque<std::string> arch_queue_;   // analyse de tous les liens morts
    int arch_total_ = 0;
    double arch_next_ = 0;

    // Essai automatique (TUBEMASTER_SELFTEST=1)
    bool selftest_ = false;
    double selftest_t0_ = 0;
    int selftest_stage_ = 0;
};

} // namespace sw
