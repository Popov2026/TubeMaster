// TubeMaster — tests du cœur (sans interface ni réseau).
#include <cstdio>
#include <string>

#include "library.hpp"
#include "yt.hpp"

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static void test_links() {
    int t = -1;
    CHECK(yt::extract_id("https://www.youtube.com/watch?v=tnaiR3oezm8") == "tnaiR3oezm8");
    CHECK(yt::extract_id("https://www.youtube.com/watch?v=xGm5kFrFrws&t=2s", &t) == "xGm5kFrFrws" && t == 2);
    CHECK(yt::extract_id("https://www.youtube.com/watch?v=ecWOvQFYL3E&t=500s", &t) == "ecWOvQFYL3E" && t == 500);
    CHECK(yt::extract_id("https://youtu.be/dQw4w9WgXcQ?t=1m30s", &t) == "dQw4w9WgXcQ" && t == 90);
    CHECK(yt::extract_id("https://www.youtube.com/shorts/abcdefghijk") == "abcdefghijk");
    CHECK(yt::extract_id("https://m.youtube.com/watch?feature=share&v=--IA-ur8Ao0") == "--IA-ur8Ao0");
    CHECK(yt::extract_id("https://www.youtube.com/embed/-7m9G8IwJ8c?autoplay=1") == "-7m9G8IwJ8c");
    CHECK(yt::extract_id("  _gTkKcMfc6w ") == "_gTkKcMfc6w");
    CHECK(yt::extract_id("https://example.com/watch?v=tnaiR3oezm8").empty());
    CHECK(yt::extract_id("https://www.youtube.com/watch?v=tooshort").empty());

    std::string file = "https://www.youtube.com/watch?v=tnaiR3oezm8\r\n"
                       "https://www.youtube.com/watch?v=B1B6_g9qRWE\r\n"
                       "\r\nVoir aussi https://youtu.be/dQw4w9WgXcQ et (https://www.youtube.com/shorts/abcdefghijk).\n"
                       "jNQXAC9IVRw\n"
                       "https://www.youtube.com/watch?v=B1B6_g9qRWE\n";
    auto l = yt::find_links(file);
    CHECK(l.size() == 6);
    CHECK(l.size() == 6 && l[0].id == "tnaiR3oezm8" && l[2].id == "dQw4w9WgXcQ" && l[3].id == "abcdefghijk" &&
          l[4].id == "jNQXAC9IVRw" && l[5].id == "B1B6_g9qRWE");
    CHECK(yt::watch_url("abcdefghijk", 30) == "https://www.youtube.com/watch?v=abcdefghijk&t=30s");
    CHECK(yt::channel_url("@MaChaine") == "https://www.youtube.com/@MaChaine");
    CHECK(yt::channel_url("MaChaine") == "https://www.youtube.com/@MaChaine");
    CHECK(yt::channel_url("UCuAXFkgsw1L7xaCfnd5JJOw") == "https://www.youtube.com/channel/UCuAXFkgsw1L7xaCfnd5JJOw");
}

static void test_oembed_archives() {
    yt::OEmbed o;
    CHECK(yt::parse_oembed(R"({"title":"Ma vid\u00e9o","author_name":"Cha\u00eene","author_url":"https://www.youtube.com/@x"})", o));
    CHECK(o.title == "Ma vidéo" && o.author == "Chaîne");
    CHECK(!yt::parse_oembed("Not Found", o));
    auto ts = yt::parse_cdx(R"([["timestamp","statuscode"],["20260711102824","200"],["20260711102836","200"]])");
    CHECK(ts.size() == 2 && ts[0] == "20260711102824");
    CHECK(yt::parse_cdx("[]").empty());
    CHECK(yt::fmt_ts("20260711102824") == "11/07/2026 10:28");

    // Page récente (données du lecteur) et page ancienne (balises meta / itemprop).
    std::string recent = R"(<html><head><title>Mon titre - YouTube</title><meta property="og:title" content="Mon titre &amp; co">
<meta itemprop="datePublished" content="2019-03-02T10:00:00-08:00"></head><body><script>var ytInitialPlayerResponse = {"videoDetails":
{"title":"Mon titre & co","author":"Ma Cha\u00eene","channelId":"UCabc","shortDescription":"Ligne 1\nLigne 2","viewCount":"1234"}};</script></body></html>)";
    yt::Archived a;
    CHECK(yt::parse_watch_page(recent, a));
    CHECK(a.title == "Mon titre & co" && a.channel == "Ma Chaîne" && a.channel_url == "https://www.youtube.com/channel/UCabc");
    CHECK(a.date == "2019-03-02" && a.views == "1234" && a.description == "Ligne 1\nLigne 2");
    std::string old = R"(<html><head><title>Vieux clip - YouTube</title><meta name="title" content="Vieux clip">
<meta name="description" content="Desc"></head><body><span itemprop="author" itemscope itemtype="http://schema.org/Person">
<link itemprop="url" href="http://www.youtube.com/user/vieux"><link itemprop="name" content="Vieux Compte"></span>
<meta itemprop="interactionCount" content="99"></body></html>)";
    yt::Archived b;
    CHECK(yt::parse_watch_page(old, b));
    CHECK(b.title == "Vieux clip" && b.channel == "Vieux Compte" && b.channel_url == "http://www.youtube.com/user/vieux");
    CHECK(b.views == "99" && b.description == "Desc");
    yt::Archived c;
    CHECK(!yt::parse_watch_page("<html><title>YouTube</title></html>", c));
    std::string y2012 = R"(<html><head><title>Me at the zoo - YouTube</title><meta name="title" content="Me at the zoo"></head><body>
<a href="/user/jawed" class="yt-uix-sessionlink yt-user-name author" rel="author" dir="ltr">jawed</a>
<span id="eow-date" class="watch-video-date" >23 avr. 2005</span><span class="watch-view-count " >12 345 678</span></body></html>)";
    yt::Archived e;
    CHECK(yt::parse_watch_page(y2012, e));
    CHECK(e.title == "Me at the zoo" && e.channel == "jawed" && e.date == "23 avr. 2005" && e.views == "12345678");
    CHECK(yt::normalize_date("23 avr. 2005") == "2005-04-23");
    CHECK(yt::normalize_date("Apr 23, 2005") == "2005-04-23");
    CHECK(yt::normalize_date("Premiered Mar 1, 2020") == "2020-03-01");
    CHECK(yt::normalize_date("Streamed live on Dec 5, 2019") == "2019-12-05");
    CHECK(yt::normalize_date("1 décembre 2019") == "2019-12-01");
    CHECK(yt::normalize_date("2009-10-24T23:57:33-07:00") == "2009-10-24");
    CHECK(yt::normalize_date("il y a 3 ans").empty());
    yt::Archived g;
    yt::parse_watch_page(R"(<title>X - YouTube</title>..."dateText":{"simpleText":"Apr 23, 2005"},...)", g);
    CHECK(yt::normalize_date(g.date) == "2005-04-23");
}

static void test_ytdlp() {
    yt::Progress p;
    CHECK(yt::parse_progress("[download]  42.3% of   12.34MiB at    1.23MiB/s ETA 00:07", p));
    CHECK(p.pct > 42.2 && p.pct < 42.4 && p.speed == "1.23MiB/s" && p.eta == "00:07");
    yt::Progress q;
    CHECK(yt::parse_progress("[download] 100% of 5.00MiB in 00:00:03", q) && q.pct == 100);
    CHECK(!yt::parse_progress("[youtube] Extracting URL", q));
    yt::Job j;
    j.url = "https://www.youtube.com/watch?v=abcdefghijk";
    j.format = yt::Format::MKV;
    j.quality = yt::Quality::P720;
    j.dest_dir = "D:\\youtube\\MKV-720p";
    j.cookies = "C:\\c.txt";
    auto a = yt::download_args(j);
    auto has = [&](const std::string& s) { for (auto& x : a) if (x == s) return true; return false; };
    CHECK(has("mkv") && has("--cookies") && has("C:\\c.txt") && has("bv*[height<=720]+ba/b[height<=720]/bv*+ba/b"));
    CHECK(!has("--js-runtimes"));
    j.js_runtime = "deno:C:\\t\\deno.exe";
    a = yt::download_args(j);
    CHECK(has("--js-runtimes") && has("deno:C:\\t\\deno.exe"));
    CHECK(a.back() == j.url && a[a.size() - 2] == "D:\\youtube\\MKV-720p/%(title)s.%(ext)s");
    j.format = yt::Format::MP3;
    j.quality = yt::Quality::Max;
    a = yt::download_args(j);
    CHECK(has("mp3") && has("-x") && has("0"));
    std::string id, title;
    CHECK(yt::parse_channel_line("tnaiR3oezm8\tUn titre", id, title) && id == "tnaiR3oezm8" && title == "Un titre");
    CHECK(!yt::parse_channel_line("ERROR: blah", id, title));
}

static void test_library() {
    tub::Library lib;
    tub::Video v;
    v.id = "tnaiR3oezm8";
    v.title = "Titre é";
    v.status = tub::Status::Removed;
    v.lists = {"Youtube", "Favoris"};
    v.arch_snaps = 2;
    v.arch_title = "Retrouvé";
    lib.videos.push_back(v);
    lib.lists = {"Youtube", "Favoris"};
    lib.cfg.dest_dir = "D:\\youtube";
    lib.cfg.format = 2;
    std::string s = tub::serialize_library(lib);
    CHECK(tub::write_file_atomic("tm_test.json", s));
    tub::Library back;
    CHECK(tub::load_library("tm_test.json", back));
    CHECK(back.videos.size() == 1 && back.videos[0].title == "Titre é" && back.videos[0].status == tub::Status::Removed);
    CHECK(back.videos[0].lists.size() == 2 && back.videos[0].arch_title == "Retrouvé" && back.cfg.format == 2);
    CHECK(back.lists.size() == 2 && back.cfg.dest_dir == "D:\\youtube");
    CHECK(tub::list_name_from_path("C:\\x\\Youtube.txt") == "Youtube" || tub::list_name_from_path("/x/Youtube.txt") == "Youtube");
    std::remove("tm_test.json");
}

int main() {
    test_links();
    test_oembed_archives();
    test_ytdlp();
    test_library();
    if (failures) { std::printf("%d échec(s)\n", failures); return 1; }
    std::printf("Tous les tests passent.\n");
    return 0;
}
