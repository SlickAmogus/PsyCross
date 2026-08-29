file(GLOB_RECURSE PSYCROSS_SRCS_C
	"*.c"
)

file(GLOB_RECURSE PSYCROSS_SRCS_CPP
	"*.cpp"
)
list(FILTER PSYCROSS_SRCS_CPP EXCLUDE REGEX "/tests/")
list(REMOVE_ITEM PSYCROSS_SRCS_CPP
	"${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PsyX_SPUAL.cpp")

# Neither mobile target has OpenAL: Android never had it, and Apple removed its
# OpenAL framework. PsyX_SPUSoftware renders the SPU itself and pushes through
# SDL2's audio sink, so the legacy AL backend leaves the build here (before
# add_library, or the removal would come too late to matter) and
# PsyX_SPUDispatch.cpp collapses onto the software one via PSYX_NO_OPENAL.
if(ANDROID OR IOS)
	list(REMOVE_ITEM PSYCROSS_SRCS_CPP
		"${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PsyX_SPULegacy.cpp")
	# PsyX_SPUSpatial.cpp routes the software SPU through OpenAL placement
	# for surround layouts, so it includes <AL/al.h> unconditionally and
	# cannot compile where there is no OpenAL at all. Its own header already
	# contracts "returns false if OpenAL is unavailable", and
	# PsyX_SPUSoftware falls through to the stereo sink on false, so the
	# stubs in that file are the documented behaviour rather than a bypass.
	list(REMOVE_ITEM PSYCROSS_SRCS_CPP
		"${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PsyX_SPUSpatial.cpp")
	# glad.c is the desktop-GL entry-point loader. Both mobile targets link
	# their GLES implementation directly (PsyX_render.h leaves USE_GLAD
	# undefined on each), so the loader is never called and its desktop-only
	# PFNGL*PROC types do not exist here.
	list(REMOVE_ITEM PSYCROSS_SRCS_C
		"${CMAKE_CURRENT_SOURCE_DIR}/src/render/glad.c")
endif()

set_source_files_properties( ${PSYCROSS_SRCS_C} PROPERTIES LANGUAGE C)
set_source_files_properties( ${PSYCROSS_SRCS_CPP} PROPERTIES LANGUAGE CXX)

# C11 uses _Static_assert, but PsyCross headers use C++ style static_assert
# Add compatibility define for C compilation
set_source_files_properties( ${PSYCROSS_SRCS_C} PROPERTIES
    COMPILE_DEFINITIONS "static_assert=_Static_assert"
)

# AppleClang treats an uppercase .C extension as C++ regardless of the LANGUAGE
# property. Force C mode explicitly so -std=gnu11 is accepted.
if(APPLE)
    set_source_files_properties( ${PSYCROSS_SRCS_C} PROPERTIES COMPILE_FLAGS "-x c")
endif()
#set_source_files_properties( "src/gte/PsyX_GTE.cpp" PROPERTIES LANGUAGE C)

add_library(psycross_static STATIC ${PSYCROSS_SRCS_C} ${PSYCROSS_SRCS_CPP})
set_target_properties(psycross_static PROPERTIES OUTPUT_NAME "psycross")
# "include/psx" is deliberately NOT public: it holds a PSX-SDK <strings.h> that
# is a guard-only stub declaring nothing, so putting it on the include path
# shadows the POSIX header for every TU built against this archive (that is why
# strcasecmp used to come up undeclared on glibc). Consumers that need bare
# <libgpu.h>-style PSX includes add the directory themselves.
target_include_directories(psycross_static PUBLIC "include")

# On ELF the host executable that links this archive is built with
# -rdynamic (so map overlay .so's can resolve game symbols at dlopen). That
# also exports every default-visibility global, and two TUs here define
# symbols whose names collide with libc and libopenal:
#   - libapi.c implements PSX file I/O as open/read/close/write/lseek/ioctl/
#     rename. Exporting these interposed libc for ALL loaded libraries —
#     Mesa's read() of the GPU's PCI sysfs `vendor` node during GL context
#     creation was dispatched to the game's read() and hung startup.
#   - PsyX_SPULegacy.cpp includes PsyX_SPUAL.cpp and holds alGenEffects/... as
#     global function-pointer
#     variables, which interposed libopenal's EFX functions.
# -rdynamic cannot export hidden-visibility symbols, so compile just these
# TUs hidden. They're still used normally within the module; nothing
# external needs them (the maps link their own copy of this archive).
if(NOT WIN32)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/psx/libapi.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PsyX_SPULegacy.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PsyX_SPUSoftware.cpp"
        PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")
endif()

if(ANDROID OR IOS)
    # SDL2 is built from source alongside this (android_port/app/jni and
    # ios_port/CMakeLists.txt each add the submodule before us), so the target
    # already exists — find_package would look for a system SDL2 that an NDK
    # sysroot or iPhoneOS SDK does not have.
    target_link_libraries(psycross_static SDL2)
else()
find_package(SDL2 REQUIRED)
target_link_libraries(psycross_static ${SDL2_LIBRARIES})
target_include_directories(psycross_static PRIVATE ${SDL2_INCLUDE_DIRS})
endif()

# The legacy AL backend is already out of the source list on Android (see the
# REMOVE_ITEM above); PSYX_NO_OPENAL is what makes the dispatch stop referring
# to it. PUBLIC so pc_port's TUs see the same collapsed contract.
if(ANDROID)
    target_compile_definitions(psycross_static PUBLIC PSYX_NO_OPENAL)
    target_link_libraries(psycross_static GLESv3 EGL android log)
elseif(IOS)
    target_compile_definitions(psycross_static PUBLIC PSYX_NO_OPENAL)
    # iOS reaches GLES through a framework, and has no EGL — SDL creates the
    # context via EAGL. The rest of the frameworks the app needs are linked at
    # the executable, not here.
    target_link_libraries(psycross_static "-framework OpenGLES")
else()
    find_package(OpenAL REQUIRED)
    if(TARGET OpenAL::OpenAL)
        target_link_libraries(psycross_static OpenAL::OpenAL)
    elseif(TARGET OpenAL)
        target_link_libraries(psycross_static OpenAL)
    else()
        target_link_libraries(psycross_static ${OPENAL_LIBRARY})
        target_include_directories(psycross_static PRIVATE ${OPENAL_INCLUDE_DIR})
    endif()
    if(OpenAL_SOURCE_DIR)
        target_include_directories(psycross_static PRIVATE ${OpenAL_BINARY_DIR} ${OpenAL_SOURCE_DIR}/include)
    endif()
    if(OPENAL_INCLUDE_DIR)
        target_include_directories(psycross_static PRIVATE ${OPENAL_INCLUDE_DIR})
    endif()
endif()

if(WIN32)
    target_link_libraries(psycross_static ole32 ksguid avrt propsys)
endif()

if(BUILD_TESTING)
    # Without this add_test() registers nothing and ctest reports "No tests
    # were found" — PsyCross has no project() of its own.
    enable_testing()

    # Each test file carries its own main(), so each builds as its own binary
    # against just the DSP translation units it exercises — none of them link
    # psycross_static, so they stay free of SDL/OpenAL/GL.
    set(PSYCROSS_SPU_CORE_SRCS
        src/audio/PsyX_SPUCore.cpp
        src/audio/PsyX_SPUReverb.cpp
        src/audio/PsyX_IdealReverb.cpp
        src/audio/PsyX_ReferenceReverb.cpp
        src/audio/PsyX_ReferenceResampler.cpp)

    function(psycross_add_audio_test name)
        add_executable(${name} ${ARGN})
        target_include_directories(${name} PRIVATE
            "include" "include/psx" "src" "src/audio")
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    psycross_add_audio_test(psycross_spu_dispatch_test
        tests/audio/PsyX_SPUDispatch_test.cpp
        src/audio/PsyX_SPUDispatch.cpp)
    psycross_add_audio_test(psycross_spu_core_test
        tests/audio/PsyX_SPUCore_test.cpp ${PSYCROSS_SPU_CORE_SRCS})
    psycross_add_audio_test(psycross_spu_enhanced_test
        tests/audio/PsyX_SPUEnhanced_test.cpp ${PSYCROSS_SPU_CORE_SRCS})
    psycross_add_audio_test(psycross_ideal_reverb_test
        tests/audio/PsyX_IdealReverb_test.cpp src/audio/PsyX_IdealReverb.cpp)
    psycross_add_audio_test(psycross_reference_reverb_test
        tests/audio/PsyX_ReferenceReverb_test.cpp src/audio/PsyX_ReferenceReverb.cpp)
    psycross_add_audio_test(psycross_reference_resampler_test
        tests/audio/PsyX_ReferenceResampler_test.cpp src/audio/PsyX_ReferenceResampler.cpp)
    psycross_add_audio_test(psycross_reference_xa_test
        tests/audio/PsyX_ReferenceXA_test.cpp
        src/audio/PsyX_ReferenceXA.cpp src/audio/PsyX_ReferenceResampler.cpp)
endif()
