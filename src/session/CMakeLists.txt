add_library(wolf_session)
add_library(wolf::session ALIAS wolf_session)

target_include_directories(wolf_session PUBLIC .)

target_link_libraries(wolf_session PUBLIC wolf::helpers)

target_sources(wolf_session
        PRIVATE
        session_machine.cpp

        PUBLIC
        session_actor.hpp
        session_effect.hpp
        session_input.hpp
        session_model.hpp
        session_state.hpp)

target_compile_features(wolf_session PUBLIC cxx_std_20)
