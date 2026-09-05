# ZMK ports of Pascal Getreuer's Select Word, Cyclotab and Orbital Mouse.
zephyr_include_directories(include)

zephyr_library()
zephyr_library_sources(src/behavior_select_word.c)
zephyr_library_sources(src/behavior_cyclotab.c)
zephyr_library_sources(src/behavior_orbital_mouse.c)
