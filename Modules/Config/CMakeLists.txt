# Modules/Config/CMakeLists.txt

add_library(
  Config STATIC
  src/skin/SkinLoader.cpp src/skin/translation/Translation.cpp src/AppPaths.cpp
  src/AppConfig.cpp src/FontPreferenceValidator.cpp)

target_include_directories(Config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(Config PUBLIC luajit::luajit 3rd_nlohmann_json 3rd_sol2
                                    Log)

mmm_add_test_executable(Config FontPreferenceValidatorTest
                        tests/FontPreferenceValidatorTest.cpp)
target_link_libraries(FontPreferenceValidatorTest PRIVATE Config)
add_test(NAME FontPreferenceValidatorTest COMMAND FontPreferenceValidatorTest)
