# title_plus, built against a Kyubu checkout: cmake -S <kyubu> -B build -DKYUBU_MODS=<this folder> (README.md).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR "Build this mod through a Kyubu checkout: cmake -S <kyubu> -B build -DKYUBU_MODS=${CMAKE_CURRENT_SOURCE_DIR}")
endif()
kyubu_native_mod(gameplay-mods ${CMAKE_CURRENT_SOURCE_DIR} title_plus title_plus/src/main.cpp)
