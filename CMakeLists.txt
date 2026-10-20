cmake_minimum_required(VERSION 3.16)
project(AudioKnowledgeExplorer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql Multimedia Concurrent Test)
find_package(PkgConfig REQUIRED)

# MeCab (optional at build time: without it only the n-gram fallback tokenizer is built)
find_path(MECAB_INCLUDE_DIR mecab.h)
find_library(MECAB_LIBRARY mecab)
if(MECAB_INCLUDE_DIR AND MECAB_LIBRARY)
    message(STATUS "MeCab: ${MECAB_LIBRARY}")
else()
    message(WARNING "MeCab not found (libmecab-dev): building with the n-gram fallback tokenizer only")
endif()
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswresample libavutil)

# whisper.cpp (built from source, CPU only, static). Point AKE_WHISPER_DIR at a checkout, or leave empty to fetch.
set(AKE_WHISPER_DIR "" CACHE PATH "whisper.cpp source directory (empty = fetch from GitHub)")
set(AKE_WHISPER_TAG "v1.7.6" CACHE STRING "whisper.cpp tag to fetch when AKE_WHISPER_DIR is empty")
set(_ake_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
    # ggml's OpenMP threads wait on each other so badly on Haiku that more threads are slower (8 threads: 4x slower than 1);
    # its own thread pool is 5x faster there (design.md measurements)
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
endif()
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
if(AKE_WHISPER_DIR)
    add_subdirectory(${AKE_WHISPER_DIR} third_party/whisper EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(whisper GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git GIT_TAG ${AKE_WHISPER_TAG})
    FetchContent_MakeAvailable(whisper)
endif()
set(BUILD_SHARED_LIBS ${_ake_shared})

# Core library (no GUI dependency)
add_library(ake_core STATIC
    src/core/IAudioPlayer.h     # Q_OBJECT interface: listed so that AUTOMOC sees it
    src/core/Database.cpp
    src/core/FolderScanner.cpp
    src/core/Library.cpp
    src/core/DemoData.cpp
    src/core/RecordingFilter.cpp
    src/core/FFmpegDecoder.cpp
    src/core/WhisperTranscriber.cpp
    src/core/TranscriptRepository.cpp
    src/core/AnalysisPipeline.cpp
    src/core/Tokenizer.cpp
    src/core/NgramTokenizer.cpp
    src/core/KeywordExtractor.cpp
    src/core/TextIndexer.cpp
    src/core/TranscriptCleaner.cpp
    src/core/ExtractiveSummarizer.cpp
    src/core/RuleBasedTitleGenerator.cpp
    src/core/AnalysisRepository.cpp
    src/core/KnowledgeBuilder.cpp
    src/core/SearchService.cpp
    src/core/RecordingDetail.cpp
    src/core/Settings.cpp
    src/core/AppPaths.cpp
    src/core/PhoneticMatch.cpp
    src/core/SpeechGate.cpp
    src/core/ScanService.cpp
    src/core/PollingFolderWatcher.cpp
    src/core/IFolderWatcher.h
    src/core/RecordingEditor.cpp
    src/core/JobScheduler.cpp
)
target_include_directories(ake_core PUBLIC src)
target_link_libraries(ake_core PUBLIC Qt6::Core Qt6::Sql Qt6::Concurrent PkgConfig::FFMPEG whisper)
if(MECAB_INCLUDE_DIR AND MECAB_LIBRARY)
    target_sources(ake_core PRIVATE src/core/MeCabTokenizer.cpp)
    target_include_directories(ake_core PRIVATE ${MECAB_INCLUDE_DIR})
    target_link_libraries(ake_core PUBLIC ${MECAB_LIBRARY})
    target_compile_definitions(ake_core PUBLIC AKE_HAVE_MECAB=1)
endif()

# Audio backend (QtMultimedia). Kept apart so the UI only depends on core/IAudioPlayer.h (Haiku: another backend).
add_library(ake_audio STATIC src/platform/QtAudioPlayer.cpp)
target_include_directories(ake_audio PUBLIC src)
target_link_libraries(ake_audio PUBLIC ake_core Qt6::Multimedia)

# UI library (shared by the app and tests)
add_library(ake_ui STATIC
    src/ui/Theme.cpp
    src/ui/MainWindow.cpp
    src/ui/RecordingListModel.cpp
    src/ui/CardDelegate.cpp
    src/ui/RowDelegate.cpp
    src/ui/CardListView.cpp
    src/ui/QueueStatusWidget.cpp
    src/ui/SearchSuggestPopup.cpp
    src/ui/FlowLayout.cpp
    src/ui/DetailPane.cpp
    src/ui/FolderDialog.cpp
    src/ui/FilterChipBar.cpp
    src/ui/SettingsDialog.cpp
    src/ui/TranscriptModel.cpp
    src/ui/TranscriptView.cpp
    src/ui/PlaybackController.cpp
    src/ui/MiniPlayer.cpp
    src/ui/SidebarWidget.cpp
    src/ui/TagPickerDialog.cpp
)
target_include_directories(ake_ui PUBLIC src)
target_link_libraries(ake_ui PUBLIC ake_core Qt6::Widgets)

add_executable(AudioKnowledgeExplorer src/main.cpp)
target_link_libraries(AudioKnowledgeExplorer PRIVATE ake_ui ake_audio)

add_executable(ake_cli src/cli.cpp)
target_link_libraries(ake_cli PRIVATE ake_core ake_audio)

enable_testing()
add_subdirectory(tests)
