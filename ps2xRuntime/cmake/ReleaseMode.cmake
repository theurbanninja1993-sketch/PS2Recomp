include(CheckIPOSupported)

# Off on Windows, and not for performance reasons. raylib exports CloseWindow and
# ShowCursor, which are also USER32 imports, so the same symbol is defined in
# libraylib.a and in the import library. Without LTO the linker only consults the
# import library for symbols nothing else defines, and it links; with LTO it
# resolves both and stops with 'duplicate symbol: CloseWindow'. One line here beats
# a flag everyone has to know about - the cross-compile in the handoff notes was
# failing without it.
if(WIN32)
    set(PS2X_LTO_DEFAULT OFF)
else()
    set(PS2X_LTO_DEFAULT ON)
endif()
option(PS2X_ENABLE_LTO "Enable link-time optimization (IPO/LTO) for Release builds" ${PS2X_LTO_DEFAULT})

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    if(MSVC)
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:
                /O2 # speed
                /Ob2 # inline aggressively
                /Oi # intrinsics
                /GL # whole program opt
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /fp:fast # fast math (graphics friendly)
                /DNDEBUG
                /arch:AVX2 # Advanced Vector Extensions 2
                /GS- # Disable Buffer Security Check (faster)
                /Qspectre- # Disable Spectre mitigations (faster)
            >
        )

        if(TARGET ${TargetName})
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /LTCG # link-time code generation
                    /OPT:REF # remove unreferenced
                    /OPT:ICF # fold identical COMDATs
                >
            )
        endif()
    endif()

    if(IPO_SUPPORTED AND PS2X_ENABLE_LTO)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    elseif(NOT PS2X_ENABLE_LTO)
        message("> LTO disabled for: ${TargetName}")
    else()
        message(WARNING "Interprocedural optimization not supported: ${ipo_error}")
    endif()
endfunction()