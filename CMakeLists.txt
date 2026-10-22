# TubeMaster — gestionnaire de liens YouTube, C++17 (SDL2 / OpenGL 3.3 / Dear ImGui / libcurl / WebView2)
cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)   # aussi pour les sous-projets (curl, SDL)
# Windows : runtime C++ statique, un seul exécutable sans DLL à fournir.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(tubemaster CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_options(/utf-8 /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

option(SW_BUILD_APP "Construire le client graphique" ON)
option(SW_BUILD_TESTS "Construire les tests (sans interface)" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- dépendances sans CMake : en-têtes et sources, versions figées
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3 GIT_SHALLOW TRUE)
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json stb)

find_package(Threads REQUIRED)

# --- cœur sans interface (réseau, analyse, stockage)
add_library(swcore STATIC src/util.cpp src/yt.cpp src/library.cpp)
target_include_directories(swcore PUBLIC src)
target_link_libraries(swcore PUBLIC nlohmann_json::nlohmann_json Threads::Threads)

if(SW_BUILD_TESTS)
  enable_testing()
  add_executable(tubemaster_tests tests/tests.cpp)
  target_link_libraries(tubemaster_tests swcore)
  add_test(NAME tubemaster_tests COMMAND tubemaster_tests)
endif()

if(NOT SW_BUILD_APP)
  return()
endif()

FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9b GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

# --- SDL2 : chemins manuels (bibliothèque système sans paquet -dev), sinon
#     compilée en statique sous Windows, sinon paquet système.
set(SW_SDL2_INCLUDE "" CACHE PATH "Dossier des en-têtes SDL2 (optionnel)")
set(SW_SDL2_LIB "" CACHE FILEPATH "Bibliothèque SDL2 (optionnel)")
if(SW_SDL2_INCLUDE AND SW_SDL2_LIB)
  add_library(sw_sdl2 INTERFACE)
  target_include_directories(sw_sdl2 INTERFACE ${SW_SDL2_INCLUDE})
  target_link_libraries(sw_sdl2 INTERFACE ${SW_SDL2_LIB})
  set(SW_SDL2_TARGETS sw_sdl2)
elseif(WIN32)
  FetchContent_Declare(sdl2 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-2.30.11 GIT_SHALLOW TRUE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_FORCE_STATIC_VCRT ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(sdl2)
  set(SW_SDL2_TARGETS SDL2::SDL2main SDL2::SDL2-static)
else()
  find_package(SDL2 REQUIRED)
  set(SW_SDL2_TARGETS SDL2::SDL2)
endif()

# --- libcurl : même logique (Windows : statique, TLS natif Schannel).
set(SW_CURL_INCLUDE "" CACHE PATH "Dossier des en-têtes curl (optionnel)")
set(SW_CURL_LIB "" CACHE FILEPATH "Bibliothèque libcurl (optionnel)")
if(SW_CURL_INCLUDE AND SW_CURL_LIB)
  add_library(sw_curl INTERFACE)
  target_include_directories(sw_curl INTERFACE ${SW_CURL_INCLUDE})
  target_link_libraries(sw_curl INTERFACE ${SW_CURL_LIB})
  set(SW_CURL_TARGETS sw_curl)
elseif(WIN32)
  FetchContent_Declare(curl GIT_REPOSITORY https://github.com/curl/curl.git GIT_TAG curl-8_11_1 GIT_SHALLOW TRUE)
  foreach(opt BUILD_SHARED_LIBS BUILD_CURL_EXE BUILD_TESTING BUILD_LIBCURL_DOCS BUILD_MISC_DOCS ENABLE_CURL_MANUAL
              CURL_USE_OPENSSL CURL_USE_LIBPSL CURL_USE_LIBSSH2 USE_LIBIDN2 CURL_ZLIB CURL_BROTLI CURL_ZSTD
              USE_NGHTTP2 CURL_DISABLE_INSTALL)
    set(${opt} OFF CACHE BOOL "" FORCE)
  endforeach()
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  set(HTTP_ONLY ON CACHE BOOL "" FORCE)
  set(ENABLE_UNICODE ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(curl)
  set(SW_CURL_TARGETS CURL::libcurl)
else()
  find_package(CURL REQUIRED)
  set(SW_CURL_TARGETS CURL::libcurl)
endif()

add_library(sw_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(sw_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# Caractères au-delà de U+FFFF (emoji des titres).
target_compile_definitions(sw_imgui PUBLIC IMGUI_USE_WCHAR32)
target_link_libraries(sw_imgui PUBLIC ${SW_SDL2_TARGETS})

add_executable(tubemaster WIN32
  src/main.cpp src/app.cpp src/ui.cpp src/net.cpp src/images.cpp src/proc.cpp src/webview_win.cpp src/gl.cpp
  src/platform.cpp src/stb_impl.cpp)
# WebView2.h : généré par widl depuis l'IDL officiel du SDK (third_party/webview2).
target_include_directories(tubemaster PRIVATE ${stb_SOURCE_DIR} third_party/webview2)
target_link_libraries(tubemaster PRIVATE swcore sw_imgui ${SW_SDL2_TARGETS} ${SW_CURL_TARGETS} ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(tubemaster PRIVATE ole32 shell32 uuid advapi32)
endif()
if(MSVC)
  target_compile_options(tubemaster PRIVATE /W3)
else()
  target_compile_options(tubemaster PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
