cmake_minimum_required(VERSION 3.22)
project(AxeEdit VERSION 1.0.191)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)

add_subdirectory(JUCE)

juce_add_gui_app(AxeEdit
    PRODUCT_NAME "Axe-Edit"
    BUNDLE_ID "local.rebuild.AxeEdit"
    COMPANY_NAME "Axe-Edit arm64 rebuild"
    ICON_BIG "${CMAKE_SOURCE_DIR}/original/Resources/icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Axe-Edit shows the level of the audio input you choose in Audio Settings on its input and output meters.")

target_sources(AxeEdit PRIVATE
    Source/Main.cpp
    Source/Model.cpp
    Source/AxeMidi.cpp
    Source/UI.cpp
    Source/PacedSysex.cpp
    Source/Librarian.cpp
    Source/ManageUI.cpp
    Source/SimTest.cpp
    Source/Extras.cpp
    Source/VirtualSource.cpp
    Source/KeepAwake.mm)

target_compile_definitions(AxeEdit PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_APPLICATION_NAME_STRING="Axe-Edit"
    JUCE_APPLICATION_VERSION_STRING="1.0.191")

target_link_libraries(AxeEdit PRIVATE
    juce::juce_gui_extra
    juce::juce_audio_devices
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags)

# The original installer's data files (skins, configs, profiles, factory banks, docs)
# are copied unmodified into the bundle; the app is driven by them exactly like the original.
add_custom_command(TARGET AxeEdit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/original/MacOS"
        "$<TARGET_BUNDLE_CONTENT_DIR:AxeEdit>/Resources/AxeData")
