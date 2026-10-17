# JAJA Engine — native audio engine for JAJA Studio (Windows: ASIO / WASAPI / DirectSound, VST3)
cmake_minimum_required(VERSION 3.22)
project(JAJAEngine VERSION 1.2.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static MSVC runtime: the user does not need to install the Visual C++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 9.0.4 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)

juce_add_gui_app(JAJAEngine
  PRODUCT_NAME "JAJAEngine"
  COMPANY_NAME "JAJA"
  VERSION 1.2.0
  ICON_BIG ${CMAKE_CURRENT_SOURCE_DIR}/icon.png)

target_sources(JAJAEngine PRIVATE src/Main.cpp src/Engine.cpp)
target_include_directories(JAJAEngine PRIVATE src)
target_compile_definitions(JAJAEngine PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_ASIO=1
  JUCE_WASAPI=1
  JUCE_DIRECTSOUND=1
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_VST=0
  JUCE_PLUGINHOST_LV2=0
  JUCE_USE_MP3AUDIOFORMAT=1
  JUCE_USE_FLAC=1
  JUCE_USE_OGGVORBIS=1
  JUCE_APPLICATION_NAME_STRING="JAJAEngine"
  JUCE_APPLICATION_VERSION_STRING="1.2.0")
target_link_libraries(JAJAEngine
  PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_devices juce::juce_audio_formats
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
