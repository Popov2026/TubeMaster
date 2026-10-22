// TubeMaster — fonctions dépendant du système (Windows / Linux).
#pragma once
#include <string>
#include <vector>

namespace sw {

std::string exe_dir();                                    // dossier de l'exécutable (UTF-8)
void open_external(const std::string& url);               // navigateur / explorateur par défaut
void open_folder(const std::string& path);                // ouvre un dossier dans l'explorateur
void show_in_folder(const std::string& file);             // explorateur, fichier sélectionné
std::string pick_folder(const std::string& title);        // vide si annulé
// Fichiers à ouvrir ; filter : « Texte|*.txt;*.csv » (libellé|motifs).
std::vector<std::string> pick_open_files(const std::string& title, const std::string& filter, bool multi);
std::string pick_save_file(const std::string& title, const std::string& default_name, const std::string& filter);
void platform_init();                                     // COM, DPI…
void platform_shutdown();

} // namespace sw
