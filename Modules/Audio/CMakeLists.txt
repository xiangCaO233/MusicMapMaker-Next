# Modules/Audio/CMakeLists.txt

add_library(Audio STATIC src/TestAudioBackend.cpp)

target_link_libraries(Audio PUBLIC ICE::IonCachyEngine)
