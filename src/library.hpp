// TubeMaster — bibliothèque de liens YouTube (listes, statut, archives) et réglages.
// Sauvegarde : tubemaster_data.json à côté du programme.
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace tub {

enum class Status { Unknown = 0, Online = 1, Removed = 2, Private = 3, Error = 4 };
const char* status_label(Status s);

struct Video {
    std::string id;
    int start = 0;                   // instant de départ du lien (&t=…)
    std::string title, channel, channel_url;
    std::string published;           // date de mise en ligne (AAAA-MM-JJ), lue sur la page de la vidéo
    Status status = Status::Unknown;
    time_t added = 0, checked = 0;
    int order = 0;                   // ordre d'import (tri « ordre d'origine »)
    std::vector<std::string> lists;  // listes (fichiers importés, chaînes, listes créées)
    std::string file;                // dernier fichier exporté
    // Retrouvé dans les archives (lien mort).
    std::string arch_title, arch_channel, arch_channel_url, arch_date, arch_first, arch_last;
    int arch_snaps = -1;             // -1 : pas encore cherché
    std::string note;
    bool checking = false;           // vérification en cours (non sauvegardé)
};

struct Settings {
    std::string dest_dir;            // dossier racine des exports
    std::string cookies;             // cookies.txt (facultatif)
    std::string tools_dir;           // dossier de yt-dlp / ffmpeg / deno (vide : à côté du programme)
    int format = 0;                  // yt::Format
    int quality = 0;                 // yt::Quality
    bool subfolders = true;          // sous-dossier par format (MKV-max, MP3…)
    bool remove_done = false;        // retirer de la file après succès
    bool pause_enabled = false;      // pause automatique (anti-blocage)
    int pause_every_min = 30, pause_sec = 60;
    int sort = 0;                    // colonne de tri
    bool sort_desc = false;
    bool include_shorts = true, include_live = true;   // import de chaîne
    bool check_after_import = true;
};

struct Library {
    std::vector<Video> videos;
    std::vector<std::string> lists;  // listes connues (ordre d'affichage)
    Settings cfg;
    int next_order = 1;
};

bool load_library(const std::string& path, Library& out);
std::string serialize_library(const Library& lib);
bool write_file_atomic(const std::string& path, const std::string& data);
std::string read_file(const std::string& path, size_t max_bytes = 64u << 20);

// Nom de liste à partir d'un fichier importé (« Youtube.txt » → « Youtube »).
std::string list_name_from_path(const std::string& path);

} // namespace tub
