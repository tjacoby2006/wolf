add_library(wolf_platform)
add_library(wolf::platform ALIAS wolf_platform)

# Headers live in src/platform/ and are included as <platform/...>, so expose the parent (src).
target_include_directories(wolf_platform PUBLIC ..)

target_sources(wolf_platform
        PRIVATE
        video/encoder_policy.cpp

        PUBLIC
        video/encoder_policy.hpp)

target_compile_features(wolf_platform PUBLIC cxx_std_20)
