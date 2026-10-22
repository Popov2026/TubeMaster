// TubeMaster — YouTube : liens, oEmbed, archives (Wayback Machine…), yt-dlp.
// Fonctions pures (sans réseau ni interface) : testées dans tests/tests.cpp.
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace yt {

// ── liens ───────────────────────────────────────────────────────────────────
bool valid_id(const std::string& id);                      // 11 caractères [A-Za-z0-9_-]
// Identifiant d'une URL YouTube (watch, youtu.be, shorts, embed, live…) ou d'un identifiant seul.
std::string extract_id(const std::string& text, int* start_sec = nullptr);
struct Link { std::string id; int start = 0; };
// Tous les liens d'un texte (fichier importé, presse-papiers), dans l'ordre, doublons compris.
std::vector<Link> find_links(const std::string& text);
std::string watch_url(const std::string& id, int start = 0);
std::string thumb_url(const std::string& id);              // miniature 320x180
std::string url_encode(const std::string& s);
// « @handle », « handle », « UC… », URL : adresse de la chaîne.
std::string channel_url(const std::string& input);

// ── statut (oEmbed : titre et chaîne ; 401/403 = privée, 404 = supprimée) ─────
std::string oembed_url(const std::string& id);
struct OEmbed { std::string title, author, author_url; };
bool parse_oembed(const std::string& body, OEmbed& out);

// ── archives ────────────────────────────────────────────────────────────────
std::string wayback_cdx_url(const std::string& id);        // captures de la page de la vidéo
std::vector<std::string> parse_cdx(const std::string& body); // horodatages des captures valides (200)
std::string wayback_raw_url(const std::string& ts, const std::string& id);   // page d'origine, sans habillage
std::string wayback_page_url(const std::string& ts, const std::string& id);  // page archivée, à regarder
std::string wayback_video_url(const std::string& id);      // vidéo archivée (si elle l'a été)
std::string wayback_calendar_url(const std::string& id);
std::string fmt_ts(const std::string& ts);                 // 20260711102824 → 11/07/2026 10:28
// Date en clair (« Apr 23, 2005 », « 23 avr. 2005 », « Premiered Mar 1, 2020 », ISO…) → AAAA-MM-JJ, vide sinon.
std::string normalize_date(const std::string& text);
struct Archived { std::string title, channel, channel_url, date, description, views; };
bool parse_watch_page(const std::string& html, Archived& out);   // vrai si un titre a été trouvé

std::string filmot_url(const std::string& id);
std::string filmot_title_url(const std::string& q);
std::string ghostarchive_url(const std::string& id);
std::string archive_today_url(const std::string& id);
std::string google_url(const std::string& q);
std::string bing_url(const std::string& q);
std::string duckduckgo_url(const std::string& q);
std::string youtube_search_url(const std::string& q);

// ── yt-dlp ──────────────────────────────────────────────────────────────────
enum class Format { MKV, MP4, MP3 };
enum class Quality { Max, P720 };
const char* format_name(Format f);
const char* quality_name(Quality q, Format f);
struct Job {
    std::string url;
    Format format = Format::MKV;
    Quality quality = Quality::Max;
    std::string dest_dir;      // dossier de sortie
    std::string cookies;       // fichier cookies.txt (facultatif)
    std::string ffmpeg_dir;    // dossier de ffmpeg (facultatif)
    std::string js_runtime;    // « deno:C:\…\deno.exe » : moteur JavaScript exigé par YouTube (yt-dlp récent)
};
std::vector<std::string> download_args(const Job& j);
constexpr const char* kFileMark = "TMFILE:";          // ligne « fichier final » imprimée par yt-dlp
struct Progress { double pct = -1; std::string speed, eta; };
bool parse_progress(const std::string& line, Progress& p);
// Liste d'une chaîne : lignes « id<TAB>titre ».
std::vector<std::string> channel_args(const std::string& url);
bool parse_channel_line(const std::string& line, std::string& id, std::string& title);

} // namespace yt
