include(CheckIPOSupported)

# Off on Windows, matching the Windows build that has always been the working one.
# It is *not* what makes a Windows link succeed: raylib exports CloseWindow and
# ShowCursor, USER32 exports both as well, and whether that is fatal depends on the
# link line rather than on LTO - the runner needs ps2_vu1_programs on it, or the
# resolver takes USER32's copy and stops. See the note in build.sh. Kept off here
# because it is the configuration the Windows build is tested in, and it was never
# established to work with LTO on.
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