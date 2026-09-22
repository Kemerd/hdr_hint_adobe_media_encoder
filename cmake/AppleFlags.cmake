# ---------------------------------------------------------------------------
# Shared Apple Clang compile/link options for every HdrHint target.
#
# Mirrors cmake/MsvcFlags.cmake in spirit: a high warning level, a
# conforming language mode and symbols in Release. Objective-C++ units use
# ARC, so Cocoa objects are reference-counted by the compiler rather than
# by hand-written retain/release.
# ---------------------------------------------------------------------------
function(hh_apply_apple_flags target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wno-missing-field-initializers   # aggregate {} initialisation is used on purpose
        -Wno-unused-parameter             # interface overrides keep documented parameter names
        -fno-common
        -fvisibility=hidden
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
        $<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-deprecated-declarations>
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:Release>:-g>)          # keep symbols in Release too (dSYM-friendly)

    # Dead-code stripping keeps the bundle small; the linker ad-hoc signs on arm64.
    target_link_options(${target} PRIVATE
        $<$<CONFIG:Release>:-Wl,-dead_strip>)
endfunction()
