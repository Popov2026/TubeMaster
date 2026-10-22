// TubeMaster — interface : liens triés, lecteur YouTube intégré, export yt-dlp,
// recherche d'origine des liens morts. Tout est dessiné par le GPU (Dear ImGui).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <SDL.h>
#include <imgui.h>

#include "app.hpp"
#include "platform.hpp"

namespace fs = std::filesystem;

namespace sw {
namespace {

constexpr ImVec4 hex(unsigned rgb, float a = 1.f) {
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
constexpr ImVec4 BG = hex(0x0f0f12), BG2 = hex(0x16161b), BG3 = hex(0x202027);
constexpr ImVec4 ACCENT = hex(0xff3d3d), ACCENT2 = hex(0x3ea6ff);
constexpr ImVec4 GREEN = hex(0x2ecc71), RED = hex(0xff5252), ORANGE = hex(0xffa726);
constexpr ImVec4 GRAY = hex(0x6b6b7b), YELLOW = hex(0xffd54f);
constexpr ImVec4 FG = hex(0xececf1), FG2 = hex(0x9a9aab);

ImU32 col(const ImVec4& c, float a = -1.f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, a < 0 ? c.w : a)); }
ImTextureID tid(unsigned t) { return (ImTextureID)(intptr_t)t; }

int resize_cb(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize(size_t(d->BufTextLen));
        d->Buf = s->data();
    }
    return 0;
}

bool input_str(const char* id, const char* hint, std::string& s, ImGuiInputTextFlags f = 0) {
    return ImGui::InputTextWithHint(id, hint, s.data(), s.capacity() + 1, f | ImGuiInputTextFlags_CallbackResize,
                                    resize_cb, &s);
}

bool tab_button(const char* label, bool active, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, active ? ACCENT : BG3);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? ACCENT : hex(0x2e2e38));
    ImGui::PushStyleColor(ImGuiCol_Text, active ? hex(0xffffff) : FG);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

bool accent_button(const char* label, ImVec4 c, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.x * 0.75f, c.y * 0.75f, c.z * 0.75f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
    ImGui::PushStyleColor(ImGuiCol_Text, hex(0xffffff));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

ImVec4 status_color(tub::Status s) {
    switch (s) {
    case tub::Status::Online: return GREEN;
    case tub::Status::Removed: return RED;
    case tub::Status::Private: return ORANGE;
    case tub::Status::Error: return GRAY;
    default: return FG2;
    }
}

const char* kSpecialDead = "\xC2\xA7" "dead";
const char* kSpecialUnknown = "\xC2\xA7" "unknown";
const char* kSpecialDone = "\xC2\xA7" "done";

void set_bounds(WebView* w, ImVec2 p0, ImVec2 size) {
    ImGuiIO& io = ImGui::GetIO();
    float sx = io.DisplayFramebufferScale.x, sy = io.DisplayFramebufferScale.y;
    w->set_bounds(int(p0.x * sx), int(p0.y * sy), int(size.x * sx), int(size.y * sy));
}

} // namespace

// ── style ────────────────────────────────────────────────────────────────────
void App::apply_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0;
    s.ChildRounding = 6;
    s.FrameRounding = 6;
    s.PopupRounding = 8;
    s.GrabRounding = 6;
    s.ScrollbarRounding = 8;
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 7);
    s.WindowBorderSize = 0;
    s.PopupBorderSize = 1;
    s.ScrollbarSize = 14;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = BG;
    c[ImGuiCol_ChildBg] = BG2;
    c[ImGuiCol_PopupBg] = hex(0x1b1b22, 0.98f);
    c[ImGuiCol_Border] = hex(0x2c2c36);
    c[ImGuiCol_FrameBg] = BG3;
    c[ImGuiCol_FrameBgHovered] = hex(0x2c2c36);
    c[ImGuiCol_FrameBgActive] = hex(0x34343f);
    c[ImGuiCol_Button] = BG3;
    c[ImGuiCol_ButtonHovered] = hex(0x30303c);
    c[ImGuiCol_ButtonActive] = ACCENT;
    c[ImGuiCol_Header] = hex(0x3ea6ff, 0.45f);
    c[ImGuiCol_HeaderHovered] = hex(0x3ea6ff, 0.25f);
    c[ImGuiCol_HeaderActive] = hex(0x3ea6ff, 0.6f);
    c[ImGuiCol_CheckMark] = ACCENT;
    c[ImGuiCol_SliderGrab] = ACCENT;
    c[ImGuiCol_Text] = FG;
    c[ImGuiCol_TextDisabled] = FG2;
    c[ImGuiCol_TableHeaderBg] = BG2;
    c[ImGuiCol_TableRowBg] = hex(0x18181e);
    c[ImGuiCol_TableRowBgAlt] = hex(0x1c1c23);
    c[ImGuiCol_TableBorderLight] = hex(0x2a2a33);
    c[ImGuiCol_PlotHistogram] = ACCENT;
    c[ImGuiCol_Separator] = hex(0x2c2c36);
    c[ImGuiCol_ScrollbarBg] = BG2;
    c[ImGuiCol_ModalWindowDimBg] = hex(0x000000, 0.6f);
    s.ScaleAllSizes(scale_);
}

// ── racine ───────────────────────────────────────────────────────────────────
void App::draw() {
    if (!style_done_) { apply_style(); style_done_ = true; }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    draw_topbar();
    switch (view_) {
    case View::Links: draw_links(); break;
    case View::Player: draw_player(); break;
    case View::Export: draw_export(); break;
    case View::Dead: draw_dead(); break;
    }
    draw_modals();
    ImGui::End();
}

void App::draw_topbar() {
    float h = 52 * scale_;
    ImGui::BeginChild("##top", ImVec2(0, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + h), col(BG2));
    dl->AddLine(ImVec2(p0.x, p0.y + h - 1), ImVec2(p0.x + W, p0.y + h - 1), col(ACCENT), 1.5f * scale_);
    ImFont* tf = fonts_.title ? fonts_.title : ImGui::GetFont();
    float ts = tf->FontSize;
    ImVec2 tp(p0.x + 16 * scale_, p0.y + (h - ts) / 2);
    // Logo : pastille rouge « ▶ » puis le nom.
    float r = ts * 0.5f;
    dl->AddRectFilled(ImVec2(tp.x, tp.y + ts * 0.1f), ImVec2(tp.x + r * 2.4f, tp.y + ts * 0.95f), col(ACCENT), r * 0.5f);
    dl->AddTriangleFilled(ImVec2(tp.x + r * 0.95f, tp.y + ts * 0.3f), ImVec2(tp.x + r * 0.95f, tp.y + ts * 0.75f),
                          ImVec2(tp.x + r * 1.6f, tp.y + ts * 0.525f), IM_COL32_WHITE);
    tp.x += r * 2.4f + 8 * scale_;
    dl->AddText(tf, ts, tp, col(FG), "Tube");
    tp.x += tf->CalcTextSizeA(ts, FLT_MAX, 0, "Tube").x;
    dl->AddText(tf, ts, tp, col(ACCENT), "Master");
    tp.x += tf->CalcTextSizeA(ts, FLT_MAX, 0, "Master").x + 26 * scale_;

    float bh = 34 * scale_;
    ImGui::SetCursorScreenPos(ImVec2(tp.x, p0.y + (h - bh) / 2));
    char lbl[96];
    std::snprintf(lbl, sizeof lbl, "▤  Liens  (%d)", int(lib_.videos.size()));
    if (tab_button(lbl, view_ == View::Links, ImVec2(0, bh))) set_view(View::Links);
    ImGui::SameLine();
    if (tab_button("▶  Lecteur", view_ == View::Player, ImVec2(0, bh))) { set_view(View::Player); ensure_webview(); }
    ImGui::SameLine();
    int waiting = 0;
    for (auto& j : jobs_) if (j.state == DlJob::Waiting || j.state == DlJob::Running) ++waiting;
    std::snprintf(lbl, sizeof lbl, waiting ? "↓  Export  (%d)###exp" : "↓  Export###exp", waiting);
    if (tab_button(lbl, view_ == View::Export, ImVec2(0, bh))) set_view(View::Export);
    ImGui::SameLine();
    if (tab_button("✝  Lien mort", view_ == View::Dead, ImVec2(0, bh))) set_view(View::Dead);

    // À droite : activité en cours.
    ImGui::SameLine(0, 24 * scale_);
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p0.y + (h - ImGui::GetFontSize()) / 2));
    if (check_total_ && (check_inflight_ || !check_queue_.empty())) {
        ImGui::TextColored(ORANGE, "Vérification %d/%d", check_done_, check_total_);
        ImGui::SameLine();
    }
    if (dl_running_) {
        if (pause_until_ > 0) ImGui::TextColored(YELLOW, "Pause (%ds)", int(pause_until_ - now_sec()));
        else if (dl_cur_ >= 0 && dl_cur_ < int(jobs_.size())) ImGui::TextColored(ACCENT2, "Export %.0f%%", jobs_[size_t(dl_cur_)].pct);
        ImGui::SameLine();
    }
    if (!arch_queue_.empty()) { ImGui::TextColored(FG2, "Archives %d/%d", arch_total_ - int(arch_queue_.size()), arch_total_); ImGui::SameLine(); }
    if (ch_running_) { ImGui::TextColored(ACCENT2, "Chaîne : %d", ch_count_); ImGui::SameLine(); }
    ImGui::NewLine();
    ImGui::EndChild();
}

// ── Liens ────────────────────────────────────────────────────────────────────
void App::draw_links() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float log_h = 92 * scale_;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * scale_, 10 * scale_));
    ImGui::BeginChild("##lside", ImVec2(300 * scale_, avail.y - log_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    draw_links_side();
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, BG);
    ImGui::BeginChild("##ltable", ImVec2(0, avail.y - log_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    draw_links_table();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::BeginChild("##llog", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    draw_log_lines(0);
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void App::draw_log_lines(float) {
    ImGui::PushFont(fonts_.small);
    for (auto& l : log_) ImGui::TextColored(FG2, "%s", l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
}

void App::draw_links_side() {
    float W = ImGui::GetContentRegionAvail().x;
    int n_dead = 0, n_unknown = 0, n_done = 0;
    std::unordered_map<std::string, int> counts;
    for (auto& v : lib_.videos) {
        if (v.status == tub::Status::Removed || v.status == tub::Status::Private) ++n_dead;
        if (v.status == tub::Status::Unknown || v.status == tub::Status::Error) ++n_unknown;
        if (!v.file.empty()) ++n_done;
        for (auto& l : v.lists) ++counts[l];
    }
    ImGui::TextColored(ACCENT, "LISTES");
    auto entry = [&](const std::string& key, const std::string& label, int n, ImVec4 c) {
        char b[256];
        std::snprintf(b, sizeof b, "%s  (%d)##%s", label.c_str(), n, key.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        if (ImGui::Selectable(b, cur_list_ == key)) { cur_list_ = key; rows_dirty_ = true; }
        ImGui::PopStyleColor();
    };
    ImGui::BeginChild("##lists", ImVec2(0, 210 * scale_), ImGuiChildFlags_Borders);
    entry("", "Tous les liens", int(lib_.videos.size()), FG);
    entry(kSpecialDead, "✝ Liens morts", n_dead, RED);
    entry(kSpecialUnknown, "Non vérifiés", n_unknown, FG2);
    entry(kSpecialDone, "↓ Exportés", n_done, ACCENT2);
    ImGui::Separator();
    std::string drop;
    for (auto& l : lib_.lists) {
        entry(l, l, counts[l], FG);
        if (ImGui::BeginPopupContextItem(("##lctx" + l).c_str())) {
            if (ImGui::MenuItem("Afficher")) { cur_list_ = l; rows_dirty_ = true; }
            if (ImGui::MenuItem("Supprimer la liste (les liens restent)")) drop = l;
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    if (!drop.empty()) {
        for (auto& v : lib_.videos) v.lists.erase(std::remove(v.lists.begin(), v.lists.end(), drop), v.lists.end());
        lib_.lists.erase(std::remove(lib_.lists.begin(), lib_.lists.end(), drop), lib_.lists.end());
        if (cur_list_ == drop) cur_list_.clear();
        rows_dirty_ = true;
        save_soon();
        log("Liste supprimée : " + drop);
    }
    ImGui::SetNextItemWidth(W - 84 * scale_);
    input_str("##newlist", "nouvelle liste…", new_list_input_);
    ImGui::SameLine();
    if (ImGui::Button("Créer", ImVec2(76 * scale_, 0)) && !trim(new_list_input_).empty()) {
        add_list(trim(new_list_input_));
        cur_list_ = trim(new_list_input_);
        new_list_input_.clear();
        rows_dirty_ = true;
        save_soon();
    }

    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "IMPORTER");
    if (accent_button("+  Importer des fichiers (.txt)…", ACCENT2, ImVec2(W, 0))) {
        auto files = pick_open_files("Importer des liens YouTube", "Listes de liens|*.txt;*.csv;*.html;*.htm;*.md", true);
        if (!files.empty()) import_files(files);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Chaque fichier devient une liste. Glisser-déposer accepté.");
    if (ImGui::Button("Coller les liens du presse-papiers", ImVec2(W, 0))) paste_links();
    ImGui::SetNextItemWidth(W - 44 * scale_);
    bool enter = input_str("##add", "lien ou identifiant…", add_input_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("+", ImVec2(36 * scale_, 0)) || enter) && !add_input_.empty()) {
        on_drop_text(add_input_);
        add_input_.clear();
    }

    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "CHAÎNE OU PLAYLIST");
    ImGui::SetNextItemWidth(W);
    input_str("##chan", "@chaîne, URL de chaîne ou de playlist", channel_input_);
    ImGui::Checkbox("Shorts", &lib_.cfg.include_shorts);
    ImGui::SameLine();
    ImGui::Checkbox("Directs", &lib_.cfg.include_live);
    ImGui::SetNextItemWidth(90 * scale_);
    ImGui::InputInt("dernières vidéos (0 = toutes)", &channel_limit_, 0);
    channel_limit_ = std::max(0, channel_limit_);
    if (accent_button(ch_running_ ? "■  Arrêter" : "Extraire les vidéos", ch_running_ ? RED : ACCENT2, ImVec2(W, 0)))
        start_channel_import();
    if (ch_running_) ImGui::TextColored(ACCENT2, "%d vidéo(s) récupérée(s)…", ch_count_);
}

void App::draw_links_table() {
    float W = ImGui::GetContentRegionAvail().x;
    // Barre : recherche et tri.
    ImGui::SetNextItemWidth(240 * scale_);
    if (input_str("##search", "Rechercher (titre, chaîne, ID)…", search_)) rows_dirty_ = true;
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(FG2, "Trier par");
    ImGui::SameLine();
    static const char* sorts[] = {"Ordre d'import", "Titre", "Chaîne", "Statut", "Date d'ajout", "Identifiant",
                                  "Date de mise en ligne"};
    ImGui::SetNextItemWidth(170 * scale_);
    if (ImGui::Combo("##sort", &lib_.cfg.sort, sorts, 7)) { rows_dirty_ = true; save_soon(); }
    ImGui::SameLine();
    if (ImGui::Button(lib_.cfg.sort_desc ? "↓ décroissant" : "↑ croissant")) { lib_.cfg.sort_desc = !lib_.cfg.sort_desc; rows_dirty_ = true; save_soon(); }
    ImGui::SameLine();
    if (accent_button("⇅  Trier", ACCENT2)) { rows_dirty_ = true; log("Liste triée par « " + std::string(sorts[lib_.cfg.sort]) + " »."); }
    ImGui::SameLine();
    ImGui::TextColored(FG2, "%d lien(s)", int(rows_.size()));

    // Actions sur la sélection.
    auto sel = selected_ids();
    ImGui::BeginDisabled(sel.empty());
    if (accent_button("▶ Lire", ACCENT)) play(sel.front());
    ImGui::SameLine();
    if (ImGui::Button("↓ Exporter")) { enqueue(sel); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ajouter à la file d'export (format choisi dans l'onglet Export)");
    ImGui::SameLine();
    if (ImGui::Button("✝ Origine")) dead_search(sel.front());
    ImGui::SameLine();
    if (ImGui::Button("↻ Vérifier")) check_ids(sel);
    ImGui::SameLine();
    if (ImGui::Button("Ranger par chaîne")) move_to_channel_lists(sel);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Déplace chaque lien sélectionné vers la liste au nom de sa chaîne.");
    ImGui::SameLine();
    if (ImGui::Button("Ajouter à une liste…")) ImGui::OpenPopup("##addto");
    if (ImGui::BeginPopup("##addto")) {
        for (auto& l : lib_.lists) if (ImGui::MenuItem(l.c_str())) add_to_list(sel, l);
        if (lib_.lists.empty()) ImGui::TextColored(FG2, "Créez d'abord une liste (panneau de gauche).");
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    bool in_list = !cur_list_.empty() && cur_list_[0] != '\xC2';
    if (in_list && ImGui::Button("Retirer de la liste")) remove_from_list(sel, cur_list_);
    if (in_list) ImGui::SameLine();
    if (ImGui::Button("Copier")) {
        std::string t;
        for (auto& id : sel) t += yt::watch_url(id) + "\r\n";
        SDL_SetClipboardText(t.c_str());
        log(std::to_string(sel.size()) + " lien(s) copié(s).");
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, RED);
    if (ImGui::Button("Supprimer")) confirm_delete_ = "sel";
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("↻ Tout vérifier")) {
        std::vector<std::string> all;
        for (size_t i : rows_) all.push_back(lib_.videos[i].id);
        check_ids(all);
    }
    ImGui::SameLine();
    if (ImGui::Button("Enregistrer (.txt)")) export_list_txt();
    if (!sel.empty()) { ImGui::SameLine(); ImGui::TextColored(ACCENT2, "%d sélectionné(s)", int(sel.size())); }

    ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                         ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit |
                         ImGuiTableFlags_Hideable;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6 * scale_, 4 * scale_));
    bool open = ImGui::BeginTable("##videos", 8, tf);
    ImGui::PopStyleVar();
    if (!open) return;
    (void)W;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_DefaultSort, 46 * scale_, 0);
    ImGui::TableSetupColumn("Titre", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 3.0f, 1);
    ImGui::TableSetupColumn("Chaîne", ImGuiTableColumnFlags_WidthStretch, 1.4f, 2);
    ImGui::TableSetupColumn("Mise en ligne", 0, 104 * scale_, 6);
    ImGui::TableSetupColumn("Statut", 0, 92 * scale_, 3);
    ImGui::TableSetupColumn("Ajouté", 0, 92 * scale_, 4);
    ImGui::TableSetupColumn("Listes", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 1.0f, 99);
    ImGui::TableSetupColumn("Identifiant", 0, 110 * scale_, 5);
    ImGui::TableHeadersRow();
    // Clic sur un en-tête : même tri que le menu « Trier par ».
    if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs()) {
        if (ss->SpecsDirty && ss->SpecsCount > 0) {
            int key = int(ss->Specs[0].ColumnUserID);
            if (key != 99) {
                lib_.cfg.sort = key;
                lib_.cfg.sort_desc = ss->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                rows_dirty_ = true;
            }
            ss->SpecsDirty = false;
        }
    }
    std::string go, origin;
    ImGuiListClipper clip;
    clip.Begin(int(rows_.size()));
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            tub::Video& v = lib_.videos[rows_[size_t(r)]];
            ImGui::PushID(v.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(FG2, "%d", r + 1);
            ImGui::TableSetColumnIndex(1);
            bool dead = v.status == tub::Status::Removed || v.status == tub::Status::Private;
            std::string title = display_title(v);
            ImGui::PushStyleColor(ImGuiCol_Text, dead ? RED : FG);
            if (ImGui::Selectable(title.c_str(), sel_.count(v.id) != 0,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                ImGuiIO& io = ImGui::GetIO();
                // Clic : un lien ; Ctrl : ajouter / retirer ; Maj : toute la plage.
                auto pos = [&](const std::string& id) {
                    for (size_t k = 0; k < rows_.size(); ++k) if (lib_.videos[rows_[k]].id == id) return int(k);
                    return -1;
                };
                int a = pos(anchor_);
                if (io.KeyShift && a >= 0) {
                    if (!io.KeyCtrl) sel_.clear();
                    for (int k = std::min(a, r); k <= std::max(a, r); ++k) sel_[lib_.videos[rows_[size_t(k)]].id] = true;
                } else if (io.KeyCtrl) {
                    if (sel_.count(v.id)) sel_.erase(v.id); else sel_[v.id] = true;
                    anchor_ = v.id;
                } else {
                    sel_.clear();
                    sel_[v.id] = true;
                    anchor_ = v.id;
                }
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { if (dead) origin = v.id; else go = v.id; }
            }
            ImGui::PopStyleColor();
            if (ImGui::BeginPopupContextItem("##vctx")) {
                if (!sel_.count(v.id)) { sel_.clear(); sel_[v.id] = true; anchor_ = v.id; }
                ImGui::TextColored(ACCENT, "%s", v.id.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("▶  Lire ici")) go = v.id;
                if (ImGui::MenuItem("↓  Exporter (file d'export)")) enqueue(selected_ids());
                if (ImGui::MenuItem("✝  Chercher l'origine (lien mort)")) origin = v.id;
                if (ImGui::MenuItem("↻  Vérifier")) check_ids(selected_ids());
                {
                    auto ids = selected_ids();
                    char lb[96];
                    std::snprintf(lb, sizeof lb, "Déplacer vers la liste de sa chaîne (%d)", int(ids.size()));
                    if (ImGui::MenuItem(lb)) move_to_channel_lists(ids);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Chaque lien va dans une liste au nom de sa chaîne (créée au besoin)\n"
                                          "et quitte la liste affichée. Liens morts : chaîne retrouvée dans les archives.");
                }
                if (ImGui::MenuItem("Copier le lien")) SDL_SetClipboardText(yt::watch_url(v.id, v.start).c_str());
                if (ImGui::MenuItem("Ouvrir dans le navigateur")) open_external(yt::watch_url(v.id, v.start));
                if (!v.file.empty() && ImGui::MenuItem("Afficher le fichier exporté")) show_in_folder(v.file);
                if (!v.channel_url.empty() && ImGui::MenuItem("Ouvrir la chaîne")) open_external(v.channel_url);
                ImGui::Separator();
                if (ImGui::MenuItem("Supprimer de la bibliothèque")) confirm_delete_ = "sel";
                ImGui::EndPopup();
            }
            // Survol : miniature.
            std::string local = thumb_path(v.id);
            std::error_code tec;
            bool has_local = fs::exists(fs::u8path(local), tec);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && (!dead || has_local)) {
                // Miniature conservée sur le disque si elle existe (utile quand la vidéo a disparu).
                const Image& im = images_->get(has_local ? "file:" + local : yt::thumb_url(v.id));
                ImGui::BeginTooltip();
                ImGui::TextColored(ACCENT2, "%s", title.c_str());
                if (im.tex) ImGui::Image(tid(im.tex), ImVec2(320 * scale_, 180 * scale_));
                else ImGui::TextColored(FG2, "miniature…");
                if (dead) ImGui::TextColored(RED, "%s — miniature conservée", tub::status_label(v.status));
                if (dead && !v.arch_title.empty()) ImGui::TextColored(YELLOW, "Archives : %s", v.arch_title.c_str());
                ImGui::EndTooltip();
            } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && dead) {
                ImGui::BeginTooltip();
                if (!v.arch_title.empty()) {
                    ImGui::TextColored(FG, "Retrouvé dans les archives :");
                    ImGui::TextColored(YELLOW, "%s", v.arch_title.c_str());
                    if (!v.arch_channel.empty()) ImGui::TextColored(FG2, "Chaîne : %s", v.arch_channel.c_str());
                    if (!v.arch_date.empty()) ImGui::TextColored(FG2, "Mise en ligne : %s", v.arch_date.c_str());
                } else if (v.arch_snaps == 0) {
                    ImGui::TextColored(FG2, "Aucune capture dans la Wayback Machine.");
                } else {
                    ImGui::TextColored(FG2, "Double-clic : chercher l'origine (archives, Filmot…).");
                }
                ImGui::EndTooltip();
            }
            ImGui::TableSetColumnIndex(2);
            const std::string& ch = v.channel.empty() ? v.arch_channel : v.channel;
            ImGui::TextColored(v.channel.empty() ? FG2 : FG, "%s", ch.c_str());
            ImGui::TableSetColumnIndex(3);
            std::string pd = display_date(v);
            ImGui::TextColored(v.published.empty() ? FG2 : FG, "%s", pd.c_str());
            if (!v.published.empty() || !v.arch_date.empty())
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(v.published.empty() ? "Date retrouvée dans les archives" : "Date de mise en ligne sur YouTube");
            ImGui::TableSetColumnIndex(4);
            ImGui::TextColored(status_color(v.status), "%s", v.checking ? "…" : tub::status_label(v.status));
            ImGui::TableSetColumnIndex(5);
            ImGui::TextColored(FG2, "%s", v.added ? fmt_local(v.added, "%d/%m/%Y").c_str() : "");
            ImGui::TableSetColumnIndex(6);
            std::string ls;
            for (auto& l : v.lists) ls += (ls.empty() ? "" : ", ") + l;
            if (!v.file.empty()) ls = "↓ " + ls;
            ImGui::TextColored(FG2, "%s", ls.c_str());
            ImGui::TableSetColumnIndex(7);
            ImGui::TextColored(GRAY, "%s", v.id.c_str());
            ImGui::PopID();
        }
    if (rows_.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(FG2, lib_.videos.empty() ? "Importez un fichier de liens (bouton à gauche ou glisser-déposer)."
                                                    : "Aucun lien dans cette vue.");
    }
    ImGui::EndTable();
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput && io.KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_A, false))
        for (size_t i : rows_) sel_[lib_.videos[i].id] = true;
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !sel_.empty())
        confirm_delete_ = "sel";
    if (!go.empty()) play(go);
    if (!origin.empty()) dead_search(origin);
}

void App::draw_modals() {
    if (!confirm_delete_.empty() && !ImGui::IsPopupOpen("Supprimer ?")) ImGui::OpenPopup("Supprimer ?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Supprimer ?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto sel = selected_ids();
        ImGui::Text("Supprimer %d lien(s) de la bibliothèque (toutes les listes) ?", int(sel.size()));
        ImGui::TextColored(FG2, "Les fichiers déjà exportés ne sont pas touchés.");
        if (accent_button("Supprimer", RED, ImVec2(120 * scale_, 0))) { remove_videos(sel); confirm_delete_.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Annuler", ImVec2(120 * scale_, 0))) { confirm_delete_.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// ── Lecteur ──────────────────────────────────────────────────────────────────
void App::draw_player() {
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float strip_w = 300 * scale_, bar_h = 54 * scale_;
    // Liste de lecture.
    ImGui::BeginChild("##plist", ImVec2(strip_w, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextColored(ACCENT, "LISTE DE LECTURE");
    ImGui::SameLine();
    ImGui::TextColored(FG2, "(%d)", int(playlist_.size()));
    std::string go;
    for (size_t i = 0; i < playlist_.size(); ++i) {
        tub::Video* v = find(playlist_[i]);
        std::string t = std::to_string(i + 1) + ". " + (v ? display_title(*v) : playlist_[i]);
        bool cur = playlist_[i] == play_id_;
        bool dead = v && (v->status == tub::Status::Removed || v->status == tub::Status::Private);
        ImGui::PushStyleColor(ImGuiCol_Text, cur ? ACCENT : dead ? RED : FG);
        if (ImGui::Selectable((t + "##p" + std::to_string(i)).c_str(), cur)) go = playlist_[i];
        ImGui::PopStyleColor();
        if (cur && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.3f);
    }
    if (playlist_.empty()) ImGui::TextColored(FG2, "Double-cliquez sur un lien\ndans l'onglet Liens.");
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    ImVec2 v0(o.x + strip_w, o.y), vsz(avail.x - strip_w, avail.y - bar_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(v0, ImVec2(v0.x + vsz.x, v0.y + vsz.y), IM_COL32(0, 0, 0, 255));
    if (web_) {
        set_bounds(web_.get(), v0, vsz);
        if (!web_->ready()) {
            const char* m = web_->failed() ? web_->error().c_str() : "Démarrage du lecteur…";
            dl->AddText(ImVec2(v0.x + 20 * scale_, v0.y + 20 * scale_), col(FG2), m);
        }
    } else {
        std::string m = web_error_.empty() ? "Lecteur intégré : Windows 10/11 avec Edge WebView2." : "Lecteur indisponible : " + web_error_;
        dl->AddText(ImVec2(v0.x + 20 * scale_, v0.y + 20 * scale_), col(FG2), m.c_str());
    }
    // Barre du bas.
    ImGui::SetCursorScreenPos(ImVec2(v0.x + 12 * scale_, v0.y + vsz.y + 10 * scale_));
    tub::Video* v = play_id_.empty() ? nullptr : find(play_id_);
    if (v) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(FG, "%s", display_title(*v).c_str());
        if (!v->channel.empty()) { ImGui::SameLine(); ImGui::TextColored(FG2, "— %s", v->channel.c_str()); }
        ImGui::SameLine(0, 20 * scale_);
    }
    if (ImGui::Button("◀ Préc.")) play_step(-1);
    ImGui::SameLine();
    if (ImGui::Button("Suiv. ▶")) play_step(+1);
    ImGui::SameLine();
    ImGui::Checkbox("Enchaîner", &autoplay_next_);
    ImGui::SameLine();
    ImGui::BeginDisabled(play_id_.empty());
    if (ImGui::Button("↓ Exporter")) enqueue({play_id_});
    ImGui::SameLine();
    if (ImGui::Button("Copier le lien")) SDL_SetClipboardText(yt::watch_url(play_id_).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Navigateur")) open_external(yt::watch_url(play_id_));
    ImGui::SameLine();
    if (ImGui::Button("Plein écran") && web_) web_->set_detached_fullscreen(true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(GRAY, "Pg.Préc / Pg.Suiv · F11 · Échap");
    if (!go.empty()) play(go);
}

// ── Export ───────────────────────────────────────────────────────────────────
void App::draw_export() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * scale_, 12 * scale_));
    ImGui::BeginChild("##eopts", ImVec2(420 * scale_, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding);
    float W = ImGui::GetContentRegionAvail().x;
    tub::Settings& c = lib_.cfg;
    bool ch = false;
    ImGui::TextColored(ACCENT, "FORMAT");
    for (int f = 0; f < 3; ++f) {
        if (f) ImGui::SameLine();
        if (tab_button(yt::format_name(yt::Format(f)), c.format == f, ImVec2(80 * scale_, 0))) { c.format = f; ch = true; }
    }
    for (int q = 0; q < 2; ++q) {
        if (q) ImGui::SameLine();
        if (tab_button(yt::quality_name(yt::Quality(q), yt::Format(c.format)), c.quality == q, ImVec2(128 * scale_, 0))) { c.quality = q; ch = true; }
    }
    ImGui::TextColored(FG2, c.format == 2 ? "Audio seul, converti en MP3." : "Vidéo + son assemblés par ffmpeg, sans réencodage.");

    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "DESTINATION");
    ImGui::SetNextItemWidth(W);
    if (input_str("##dest", "dossier", c.dest_dir)) ch = true;
    if (ImGui::Button("Parcourir…")) { std::string d = pick_folder("Dossier des exports"); if (!d.empty()) { c.dest_dir = d; ch = true; } }
    ImGui::SameLine();
    if (ImGui::Button("Ouvrir")) { std::error_code ec; fs::create_directories(fs::u8path(c.dest_dir), ec); open_folder(c.dest_dir); }
    ImGui::SameLine();
    if (ImGui::Checkbox("Sous-dossier par format", &c.subfolders)) ch = true;

    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "COOKIES YOUTUBE (facultatif)");
    ImGui::SetNextItemWidth(W);
    if (input_str("##cookies", "cookies.txt (format Netscape)", c.cookies)) ch = true;
    if (ImGui::Button("Choisir le fichier…")) {
        auto f = pick_open_files("Fichier cookies.txt", "Cookies|*.txt", false);
        if (!f.empty()) { c.cookies = f[0]; ch = true; }
    }
    ImGui::SameLine();
    if (!c.cookies.empty() && ImGui::Button("Retirer")) { c.cookies.clear(); ch = true; }
    ImGui::TextColored(FG2, "Utile si YouTube demande « confirmez que vous\nn'êtes pas un robot » ou pour les vidéos réservées.");

    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "OUTILS");
    auto tool_row = [&](const char* name, const char* why) {
        std::string p = tool(name);
        auto busy = tool_busy_.find(name);
        ImGui::AlignTextToFramePadding();
        if (!p.empty()) ImGui::TextColored(GREEN, "✓ %s", name);
        else ImGui::TextColored(RED, "✗ %s", name);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", why, p.empty() ? "introuvable" : p.c_str());
        ImGui::SameLine(110 * scale_);
        if (busy != tool_busy_.end()) ImGui::TextColored(ORANGE, "%s", busy->second.c_str());
        else if (p.empty()) { if (ImGui::SmallButton((std::string("Télécharger##") + name).c_str())) download_tool(name); }
        else if (std::strcmp(name, "yt-dlp") == 0) {
            ImGui::TextColored(FG2, "%s", ytdlp_version_.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Mettre à jour")) update_ytdlp();
        } else {
            ImGui::TextColored(FG2, "%s", fs::u8path(p).parent_path().u8string().c_str());
        }
    };
    tool_row("yt-dlp", "Télécharge les vidéos.");
    tool_row("ffmpeg", "Assemble l'image et le son (MKV/MP4), convertit en MP3.");
    tool_row("deno", "Moteur JavaScript demandé par les versions récentes de yt-dlp pour YouTube.");
    ImGui::TextColored(FG2, "Dossier des outils :");
    ImGui::SetNextItemWidth(W - 100 * scale_);
    if (input_str("##tools", base_dir_.c_str(), c.tools_dir)) ch = true;
    ImGui::SameLine();
    if (ImGui::Button("Choisir…")) { std::string d = pick_folder("Dossier de yt-dlp / ffmpeg"); if (!d.empty()) { c.tools_dir = d; ch = true; refresh_tools(); } }

    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "OPTIONS");
    if (ImGui::Checkbox("Pause automatique toutes les", &c.pause_enabled)) ch = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * scale_);
    if (ImGui::InputInt("min##pe", &c.pause_every_min, 0)) { c.pause_every_min = std::clamp(c.pause_every_min, 1, 999); ch = true; }
    ImGui::Indent();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(FG2, "pendant");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * scale_);
    if (ImGui::InputInt("secondes##ps", &c.pause_sec, 0)) { c.pause_sec = std::clamp(c.pause_sec, 1, 9999); ch = true; }
    ImGui::Unindent();
    if (ImGui::Checkbox("Retirer de la file après succès", &c.remove_done)) ch = true;
    ImGui::EndChild();
    if (ch) save_soon();

    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, BG);
    ImGui::BeginChild("##equeue", ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding);
    // Ajout direct de liens.
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 330 * scale_);
    bool enter = input_str("##eadd", "coller un ou plusieurs liens (YouTube ou autre site)…", export_url_input_,
                           ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Ajouter") || enter) && !trim(export_url_input_).empty()) {
        auto links = yt::find_links(export_url_input_);
        if (!links.empty()) {
            std::vector<std::string> ids;
            for (auto& l : links) { if (!find(l.id)) import_text(yt::watch_url(l.id), std::string()); ids.push_back(l.id); }
            enqueue(ids);
        } else {
            enqueue_url(trim(export_url_input_), std::string());    // autre site : yt-dlp en gère des centaines
        }
        export_url_input_.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Coller")) {
        char* t = SDL_GetClipboardText();
        export_url_input_ = t ? t : "";
        SDL_free(t);
    }
    ImGui::SameLine();
    ImGui::TextColored(FG2, "ou « ↓ Exporter » depuis Liens");

    if (!dl_running_) {
        if (accent_button("▶  Lancer", GREEN, ImVec2(150 * scale_, 0))) start_downloads();
    } else {
        if (accent_button("■  Stop", RED, ImVec2(150 * scale_, 0))) stop_downloads();
    }
    ImGui::SameLine();
    if (ImGui::Button("Vider les terminés"))
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const DlJob& j) { return j.state == DlJob::Done; }), jobs_.end());
    ImGui::SameLine();
    ImGui::BeginDisabled(dl_running_);
    if (ImGui::Button("Tout vider")) jobs_.clear();
    ImGui::SameLine();
    if (ImGui::Button("Relancer les échecs"))
        for (auto& j : jobs_) if (j.state == DlJob::Failed || j.state == DlJob::Stopped) { j.state = DlJob::Waiting; j.pct = 0; j.error.clear(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Copier la console")) {
        std::string all;
        for (auto& l : console_) all += l + "\r\n";
        SDL_SetClipboardText(all.c_str());
        log("Console copiée dans le presse-papiers (" + std::to_string(console_.size()) + " lignes).");
    }
    if (start_pending_) { ImGui::SameLine(); ImGui::TextColored(ORANGE, "Préparation des outils…"); }
    if (pause_until_ > 0) { ImGui::SameLine(); ImGui::TextColored(YELLOW, "Pause : reprise dans %d s", int(pause_until_ - now_sec())); }

    float console_h = 210 * scale_;
    ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                         ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    int remove = -1;
    if (ImGui::BeginTable("##jobs", 6, tf, ImVec2(0, ImGui::GetContentRegionAvail().y - console_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Vidéo", ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn("Format", 0, 130 * scale_);
        ImGui::TableSetupColumn("État", 0, 90 * scale_);
        ImGui::TableSetupColumn("Progression", 0, 170 * scale_);
        ImGui::TableSetupColumn("Vitesse / reste", 0, 150 * scale_);
        ImGui::TableSetupColumn("Fichier", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < jobs_.size(); ++i) {
            DlJob& j = jobs_[i];
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Selectable(j.title.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
            if (ImGui::BeginPopupContextItem("##jctx")) {
                if (ImGui::MenuItem("Retirer de la file", nullptr, false, j.state != DlJob::Running)) remove = int(i);
                if (ImGui::MenuItem("Relancer", nullptr, false, j.state == DlJob::Failed || j.state == DlJob::Stopped)) { j.state = DlJob::Waiting; j.error.clear(); }
                if (!j.file.empty() && ImGui::MenuItem("Afficher le fichier")) show_in_folder(j.file);
                if (ImGui::MenuItem("Copier le lien")) SDL_SetClipboardText(j.url.c_str());
                ImGui::EndPopup();
            }
            if (!j.error.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", j.error.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(FG2, "%s %s", yt::format_name(j.format), yt::quality_name(j.quality, j.format));
            ImGui::TableSetColumnIndex(2);
            static const char* st[] = {"En attente", "En cours", "Terminé", "Erreur", "Arrêté"};
            ImVec4 sc = j.state == DlJob::Done ? GREEN : j.state == DlJob::Failed ? RED : j.state == DlJob::Running ? ACCENT2 : FG2;
            ImGui::TextColored(sc, "%s", st[j.state]);
            ImGui::TableSetColumnIndex(3);
            char pb[16];
            std::snprintf(pb, sizeof pb, "%.1f%%", j.pct);
            ImGui::ProgressBar(float(j.pct / 100.0), ImVec2(-1, 0), pb);
            ImGui::TableSetColumnIndex(4);
            if (j.state == DlJob::Running && j.pct > 0) ImGui::TextColored(FG2, "%s  %s", j.speed.c_str(), j.eta.c_str());
            else if (j.state == DlJob::Running) {
                // Avant la première progression : l'étape en cours (extraction, nouvel essai…).
                ImGui::TextColored(GRAY, "%s", j.status.empty() ? "démarrage…" : j.status.c_str());
                if (!j.status.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", j.status.c_str());
            }
            ImGui::TableSetColumnIndex(5);
            if (!j.file.empty()) {
                std::string fn = fs::u8path(j.file).filename().u8string();
                if (ImGui::SmallButton(("Afficher##f" + std::to_string(i)).c_str())) show_in_folder(j.file);
                ImGui::SameLine();
                ImGui::TextColored(FG2, "%s", fn.c_str());
            }
            ImGui::PopID();
        }
        if (jobs_.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(FG2, "File vide : sélectionnez des liens dans l'onglet Liens puis « ↓ Exporter ».");
        }
        ImGui::EndTable();
    }
    if (remove >= 0) jobs_.erase(jobs_.begin() + remove);
    // Console de yt-dlp.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hex(0x050507));
    ImGui::BeginChild("##console", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(fonts_.small);
    for (auto& l : console_) {
        ImVec4 cc = l.find("ERROR") != std::string::npos || l.rfind("⚠", 0) == 0 ? RED
                    : l.rfind("✅", 0) == 0 ? GREEN : hex(0x7cf29a);
        ImGui::TextColored(cc, "%s", l.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ── Lien mort ────────────────────────────────────────────────────────────────
void App::draw_dead() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float left_w = std::max(420 * scale_, avail.x * 0.4f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * scale_, 12 * scale_));
    ImGui::BeginChild("##dleft", ImVec2(left_w, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding);
    float W = ImGui::GetContentRegionAvail().x;
    ImGui::TextColored(ACCENT, "ORIGINE D'UN LIEN MORT");
    ImGui::SetNextItemWidth(W - 110 * scale_);
    bool enter = input_str("##dead", "lien ou identifiant YouTube", dead_input_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (accent_button("Chercher", ACCENT, ImVec2(102 * scale_, 0)) || enter) dead_search(dead_input_);
    int n_dead = 0, n_todo = 0;
    for (auto& v : lib_.videos)
        if (v.status == tub::Status::Removed || v.status == tub::Status::Private) { ++n_dead; if (v.arch_snaps < 0) ++n_todo; }
    if (arch_queue_.empty()) {
        char b[96];
        std::snprintf(b, sizeof b, "Analyser les %d lien(s) mort(s) de la bibliothèque", n_todo);
        ImGui::BeginDisabled(n_todo == 0);
        if (ImGui::Button(b, ImVec2(W, 0))) analyze_all_dead();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cherche le titre et la chaîne d'origine dans la Wayback Machine,\n"
                                                      "pour chaque lien mort pas encore analysé (%d au total).", n_dead);
    } else {
        ImGui::ProgressBar(float(arch_total_ - int(arch_queue_.size())) / float(std::max(1, arch_total_)), ImVec2(W - 90 * scale_, 0));
        ImGui::SameLine();
        if (ImGui::Button("Arrêter")) arch_queue_.clear();
    }
    ImGui::Separator();

    if (!dead_) {
        ImGui::TextWrapped("Collez un lien YouTube supprimé ou privé. TubeMaster interroge YouTube et la Wayback Machine "
                           "(titre, chaîne, date retrouvés dans les pages archivées), puis ouvre Filmot, Ghostarchive, "
                           "archive.today… dans le navigateur à droite.");
    } else {
        DeadSearch& d = *dead_;
        ImGui::TextColored(FG2, "Identifiant :");
        ImGui::SameLine();
        ImGui::TextColored(FG, "%s", d.id.c_str());
        // YouTube
        ImGui::TextColored(FG2, "YouTube :");
        ImGui::SameLine();
        if (d.yt_status < 0) ImGui::TextColored(FG2, "…");
        else if (d.yt_status == 200) ImGui::TextColored(GREEN, "en ligne — %s", d.oembed.title.c_str());
        else if (d.yt_status == 401 || d.yt_status == 403) ImGui::TextColored(ORANGE, "privée ou intégration désactivée");
        else if (d.yt_status == 404 || d.yt_status == 400) ImGui::TextColored(RED, "supprimée / introuvable");
        else ImGui::TextColored(GRAY, "pas de réponse (%d)", d.yt_status);
        // Wayback
        ImGui::TextColored(FG2, "Wayback Machine :");
        ImGui::SameLine();
        if (!d.cdx_done) ImGui::TextColored(FG2, "recherche des captures…");
        else if (!d.cdx_error.empty()) ImGui::TextColored(ORANGE, "erreur (%s) — réessayez dans un moment", d.cdx_error.c_str());
        else if (d.snaps.empty()) ImGui::TextColored(FG2, "aucune capture de la page");
        else ImGui::TextColored(GREEN, "%d capture(s), du %s au %s", int(d.snaps.size()), yt::fmt_ts(d.snaps.front()).c_str(),
                                yt::fmt_ts(d.snaps.back()).c_str());
        if (!d.snaps.empty()) {
            ImGui::BeginChild("##snaps", ImVec2(0, std::min(150 * scale_, 26 * scale_ * float(d.snaps.size()) + 8)), ImGuiChildFlags_Borders);
            for (size_t i = 0; i < d.snaps.size(); ++i) {
                std::string l = yt::fmt_ts(d.snaps[i]) + "##s" + std::to_string(i);
                if (ImGui::Selectable(l.c_str(), d.snaps[i] == d.found_ts)) browse(yt::wayback_page_url(d.snaps[i], d.id));
            }
            ImGui::EndChild();
        }
        // Infos retrouvées
        ImGui::Spacing();
        ImGui::TextColored(ACCENT, "RETROUVÉ");
        if (d.cdx_done && !d.page_done) ImGui::TextColored(FG2, "lecture des pages archivées (%d)…", d.page_tries);
        const yt::Archived& a = d.found;
        std::string title = !a.title.empty() ? a.title : (d.yt_status == 200 ? d.oembed.title : std::string());
        if (title.empty() && d.page_done) ImGui::TextColored(FG2, "Pas de titre retrouvé automatiquement : essayez les sources ci-dessous.");
        if (!title.empty()) {
            ImGui::TextColored(FG2, "Titre :");
            ImGui::SameLine();
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(YELLOW, "%s", title.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::SmallButton("Copier le titre")) SDL_SetClipboardText(title.c_str());
        }
        if (!a.channel.empty()) {
            ImGui::TextColored(FG2, "Chaîne :");
            ImGui::SameLine();
            ImGui::TextColored(FG, "%s", a.channel.c_str());
            if (!a.channel_url.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("ouvrir")) browse(a.channel_url);
            }
        }
        if (!a.date.empty()) { ImGui::TextColored(FG2, "Mise en ligne :"); ImGui::SameLine(); ImGui::TextColored(FG, "%s", a.date.c_str()); }
        if (!a.views.empty()) { ImGui::TextColored(FG2, "Vues :"); ImGui::SameLine(); ImGui::TextColored(FG, "%s", a.views.c_str()); }
        if (!d.found_ts.empty()) ImGui::TextColored(GRAY, "d'après la capture du %s", yt::fmt_ts(d.found_ts).c_str());
        if (!a.description.empty()) {
            ImGui::BeginChild("##desc", ImVec2(0, 110 * scale_), ImGuiChildFlags_Borders);
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(FG2, "%s", a.description.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
        }
        if (!title.empty()) {
            ImGui::TextColored(FG2, "Chercher ce titre (réenvoi, copie) :");
            if (ImGui::Button("YouTube")) browse(yt::youtube_search_url(title));
            ImGui::SameLine();
            if (ImGui::Button("Filmot##t")) browse(yt::filmot_title_url(title));
            ImGui::SameLine();
            if (ImGui::Button("Google##t")) browse(yt::google_url("\"" + title + "\""));
            ImGui::SameLine();
            if (ImGui::Button("Bing##t")) browse(yt::bing_url("\"" + title + "\""));
        }
        ImGui::Spacing();
        ImGui::TextColored(ACCENT, "SOURCES (par identifiant)");
        struct Src { const char* l; std::string u; const char* tip; };
        Src srcs[] = {
            {"Filmot", yt::filmot_url(d.id), "Base de métadonnées et sous-titres de vidéos YouTube, y compris supprimées."},
            {"Vidéo archivée", yt::wayback_video_url(d.id), "La vidéo elle-même, si la Wayback Machine l'a conservée."},
            {"Calendrier Wayback", yt::wayback_calendar_url(d.id), "Toutes les captures de la page de la vidéo."},
            {"Ghostarchive", yt::ghostarchive_url(d.id), "Archives de vidéos YouTube."},
            {"archive.today", yt::archive_today_url(d.id), "Captures archive.ph / archive.is."},
            {"Google", yt::google_url("\"" + d.id + "\""), "Pages qui citent l'identifiant."},
            {"Bing", yt::bing_url("\"" + d.id + "\""), ""},
            {"DuckDuckGo", yt::duckduckgo_url("\"" + d.id + "\""), ""},
        };
        float x = 0;
        for (auto& s : srcs) {
            float bw = ImGui::CalcTextSize(s.l).x + ImGui::GetStyle().FramePadding.x * 2;
            if (x > 0 && x + bw > W) x = 0;
            else if (x > 0) ImGui::SameLine();
            if (ImGui::Button(s.l)) browse(s.u);
            if (s.tip[0] && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.tip);
            x += bw + ImGui::GetStyle().ItemSpacing.x;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // Navigateur intégré (sites protégés comme Filmot : la vérification se fait comme dans un navigateur).
    ImGui::SameLine(0, 0);
    ImVec2 b0 = ImGui::GetCursorScreenPos();
    float bw = avail.x - left_w, bar = 42 * scale_;
    ImGui::SetCursorScreenPos(ImVec2(b0.x + 8 * scale_, b0.y + 6 * scale_));
    ImGui::BeginGroup();
    bool have = sweb_ != nullptr;
    ImGui::BeginDisabled(!have);
    if (ImGui::Button("◀")) sweb_->run_script("history.back()");
    ImGui::SameLine();
    if (ImGui::Button("▶")) sweb_->run_script("history.forward()");
    ImGui::SameLine();
    if (ImGui::Button("↻")) sweb_->run_script("location.reload()");
    ImGui::SameLine();
    if (ImGui::Button("Navigateur externe") && !sweb_url_.empty()) open_external(sweb_url_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(FG2, "%s", sweb_url_.empty() ? "Choisissez une source à gauche." : sweb_url_.substr(0, 110).c_str());
    ImGui::EndGroup();
    ImVec2 w0(b0.x, b0.y + bar), wsz(bw, avail.y - bar);
    ImGui::GetWindowDrawList()->AddRectFilled(w0, ImVec2(w0.x + wsz.x, w0.y + wsz.y), col(BG2));
    if (sweb_) set_bounds(sweb_.get(), w0, wsz);
}

// ── essai automatique ────────────────────────────────────────────────────────
// TUBEMASTER_SELFTEST=1 : import d'un fichier de liens, vérification, lecture,
// recherche d'un lien mort, export de fichiers de test ; tout est noté dans le journal.
void App::selftest_step() {
    double t = now_sec() - selftest_t0_;
    auto counts = [this] {
        int on = 0, dead = 0, unk = 0;
        for (auto& v : lib_.videos) {
            if (v.status == tub::Status::Online) ++on;
            else if (v.status == tub::Status::Removed || v.status == tub::Status::Private) ++dead;
            else ++unk;
        }
        return std::to_string(lib_.videos.size()) + " liens, " + std::to_string(on) + " en ligne, " + std::to_string(dead) +
               " morts, " + std::to_string(unk) + " inconnus";
    };
    if (selftest_stage_ == 0 && t > 2) {
        selftest_stage_ = 1;
        if (const char* f = std::getenv("TUBEMASTER_SELFTEST_FILE")) import_files({f});
        // Outils absents : l'export les installe lui-même (comme chez l'utilisateur).
        lib_.cfg.sort = 1;
        rows_dirty_ = true;
        diag("SELFTEST import : " + counts());
    } else if (selftest_stage_ == 1 && t > 14) {
        selftest_stage_ = 2;
        diag("SELFTEST vérification : " + counts());
        for (auto& v : lib_.videos)
            diag("SELFTEST   " + v.id + " " + tub::status_label(v.status) + " | " + display_title(v) + " | " + v.channel);
        for (size_t i : rows_) if (lib_.videos[i].status == tub::Status::Online) { play(lib_.videos[i].id); break; }
        diag("SELFTEST lecture de " + play_id_);
    } else if (selftest_stage_ == 2 && t > 34) {
        selftest_stage_ = 3;
        if (web_)
            web_->eval("(function(){var v=document.querySelector('video');return (document.title||'')+' | '+(v?(v.videoWidth+'x'+v.videoHeight+' t='+"
                       "v.currentTime.toFixed(1)+(v.paused?' pause':' lecture')):'aucune vidéo');})()",
                       [](const std::string& r) { diag("SELFTEST lecteur : " + r); });
        else diag("SELFTEST lecteur absent : " + web_error_);
    } else if (selftest_stage_ == 3 && t > 38) {
        selftest_stage_ = 4;
        const char* dead = std::getenv("TUBEMASTER_SELFTEST_DEAD");
        dead_search(dead ? dead : "-7m9G8IwJ8c");
    } else if (selftest_stage_ == 4 && (t > 80 || (dead_ && dead_->page_done && t > 50))) {
        selftest_stage_ = 5;
        if (dead_) {
            diag("SELFTEST lien mort " + dead_->id + " : YouTube " + std::to_string(dead_->yt_status) + ", " +
                 std::to_string(dead_->snaps.size()) + " capture(s)" + (dead_->cdx_error.empty() ? "" : " erreur " + dead_->cdx_error) +
                 ", titre « " + dead_->found.title + " », chaîne « " + dead_->found.channel + " », date " + dead_->found.date);
            browse(yt::filmot_url(dead_->id));
        }
        dead_search("jNQXAC9IVRw", false);       // vidéo ancienne : captures de 2005-2010 (analyseur d'archives)
    } else if (selftest_stage_ == 5 && t > 100) {
        if (dead_)
            diag("SELFTEST archives " + dead_->id + " : " + std::to_string(dead_->snaps.size()) + " capture(s), titre « " +
                 dead_->found.title + " », chaîne « " + dead_->found.channel + " », date " + dead_->found.date +
                 ", vues " + dead_->found.views + ", capture " + dead_->found_ts);
        selftest_stage_ = 6;
        if (sweb_)
            sweb_->eval("(function(){return document.title+' | '+location.href;})()",
                        [](const std::string& r) { diag("SELFTEST navigateur (Filmot) : " + r); });
        // Export : fichiers de test (sites autres que YouTube, qui bloque les serveurs).
        lib_.cfg.dest_dir = (fs::u8path(base_dir_) / "exports_test").u8string();
        if (const char* m = std::getenv("TUBEMASTER_SELFTEST_MEDIA")) {
            lib_.cfg.format = 0; lib_.cfg.quality = 1;
            enqueue_url(m, "Essai MKV 720p");
            lib_.cfg.format = 1; lib_.cfg.quality = 0;
            enqueue_url(m, "Essai MP4 max");
        }
        if (const char* a = std::getenv("TUBEMASTER_SELFTEST_AUDIO")) {
            lib_.cfg.format = 2; lib_.cfg.quality = 0;
            enqueue_url(a, "Essai MP3");
        }
        // YouTube : courte vidéo en MP3 et en MKV 720p (sans cookies, parfois refusé aux serveurs).
        lib_.cfg.format = 2; lib_.cfg.quality = 0;
        enqueue({"jNQXAC9IVRw"});
        lib_.cfg.format = 0; lib_.cfg.quality = 1;
        enqueue({"jNQXAC9IVRw"});
        set_view(View::Export);
        start_downloads();
        diag(std::string("SELFTEST outils avant l'export : yt-dlp ") + tool("yt-dlp") + ", ffmpeg " + tool("ffmpeg") + ", deno " + tool("deno"));
        diag(std::string("SELFTEST export lancé, yt-dlp ") + (tool("yt-dlp").empty() ? "ABSENT" : ytdlp_version_) +
             ", ffmpeg " + (tool("ffmpeg").empty() ? "ABSENT" : "présent"));
    } else if (selftest_stage_ == 6 && (t > 175 || (!dl_running_ && !start_pending_ && t > 110))) {
        selftest_stage_ = 7;
        for (auto& j : jobs_) {
            std::error_code ec;
            auto sz = j.file.empty() ? 0 : fs::file_size(fs::u8path(j.file), ec);
            static const char* st[] = {"attente", "en cours", "terminé", "ERREUR", "arrêté"};
            diag("SELFTEST export « " + j.title + " » " + yt::format_name(j.format) + " : " + st[j.state] + ", " +
                 (j.file.empty() ? "pas de fichier" : j.file + " (" + std::to_string(ec ? 0 : sz) + " octets)") +
                 (j.error.empty() ? "" : " — " + j.error.substr(0, 160)));
        }
    } else if (selftest_stage_ == 7 && t > 176) {
        selftest_stage_ = 8;
        // Miniatures conservées, puis rangement de tous les liens par chaîne.
        int thumbs = 0;
        for (auto& v : lib_.videos) { std::error_code ec; if (fs::exists(fs::u8path(thumb_path(v.id)), ec)) ++thumbs; }
        diag("SELFTEST miniatures conservées : " + std::to_string(thumbs));
        for (auto& v : lib_.videos) diag("SELFTEST date de mise en ligne " + v.id + " : " + display_date(v));
        cur_list_.clear();
        rows_dirty_ = true;
        rebuild_rows();
        sel_.clear();
        for (size_t i : rows_) sel_[lib_.videos[i].id] = true;
        move_to_channel_lists(selected_ids());
        std::string ls;
        for (auto& l : lib_.lists) ls += l + " ; ";
        diag("SELFTEST listes après rangement par chaîne : " + ls);
        set_view(View::Links);
        cur_list_ = kSpecialDead;
        rows_dirty_ = true;
    } else if (selftest_stage_ == 8 && t > 184) {
        selftest_stage_ = 9;
        diag("SELFTEST fin : " + counts());
        request_quit();
    }
}

} // namespace sw
