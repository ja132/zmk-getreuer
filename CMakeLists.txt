# ZMK ports of Pascal Getreuer's Select Word, Cyclotab and Orbital Mouse.
#
# Behavior sources must be compiled INTO the `app` target rather than into a
# standalone zephyr_library(). `drivers/behavior.h` lives in zmk/app/include,
# which zmk/app/CMakeLists.txt declares as:
#     target_include_directories(app PRIVATE include)
# A separate zephyr_library() is not `app`, so it never receives that include
# path and the build dies with "fatal error: drivers/behavior.h: No such file
# or directory". This is true of upstream ZMK and MoErgo's fork alike, and is
# why every other third-party ZMK behavior module uses target_sources(app ...).
#
# The _ifdef form also restores the "unused behaviors cost nothing" property:
# each CONFIG_ symbol depends on its DT_HAS_*_ENABLED, so a behavior nothing
# references is not compiled at all.
zephyr_include_directories(include)

target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_SELECT_WORD   app PRIVATE src/behavior_select_word.c)
target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_CYCLOTAB      app PRIVATE src/behavior_cyclotab.c)
target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_ORBITAL_MOUSE app PRIVATE src/behavior_orbital_mouse.c)
