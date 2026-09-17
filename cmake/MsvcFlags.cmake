# ---------------------------------------------------------------------------
# Shared MSVC compile/link options for every HdrHint target.
#
# Everything is UTF-8 (sources and execution charset), warnings are at /W4,
# and the non-conforming compiler extensions are switched off so the code
# stays portable to future MSVC releases.
# ---------------------------------------------------------------------------
function(hh_apply_msvc_flags target)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        STRICT
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        NTDDI_VERSION=0x0A000006)

    target_compile_options(${target} PRIVATE
        /W4                 # high warning level
        /permissive-        # standards conformance
        /utf-8              # source + execution charset
        /EHsc               # C++ exceptions, no SEH interop
        /Zc:preprocessor    # conforming preprocessor
        /Zc:__cplusplus     # report the real __cplusplus value
        /Zc:inline
        /guard:cf           # control-flow guard
        /MP                 # parallel compilation
        /bigobj
        /wd4324             # structure was padded due to alignment specifier (harmless)
        /wd4458             # declaration hides class member (used intentionally in setters)
        $<$<CONFIG:Release>:/Zi>   # keep symbols in Release too
        $<$<CONFIG:Release>:/O2>)

    target_link_options(${target} PRIVATE
        /guard:cf
        $<$<CONFIG:Release>:/DEBUG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>)

    # Static runtime keeps the exe self-contained (no VC++ redistributable needed).
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endfunction()
