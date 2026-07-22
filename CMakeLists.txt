file(GLOB_RECURSE PSYCROSS_SRCS_C
	"*.c"
)

file(GLOB_RECURSE PSYCROSS_SRCS_CPP
	"*.cpp"
)
list(FILTER PSYCROSS_SRCS_CPP EXCLUDE REGEX "/tests/")
list(REMOVE_ITEM PSYCROSS_SRCS_CPP
	"${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PsyX_SPUAL.cpp")

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
target_include_directories(psycross_static PUBLIC "include" "include/psx")

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

find_package(SDL2 REQUIRED)
target_link_libraries(psycross_static ${SDL2_LIBRARIES})
target_include_directories(psycross_static PRIVATE ${SDL2_INCLUDE_DIRS})

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

if(WIN32)
    target_link_libraries(psycross_static ole32 ksguid avrt propsys)
endif()

if(BUILD_TESTING)
    add_executable(psycross_spu_dispatch_test
        tests/audio/PsyX_SPUDispatch_test.cpp
        src/audio/PsyX_SPUDispatch.cpp)
    target_include_directories(psycross_spu_dispatch_test PRIVATE "include" "include/psx" "src")
    add_test(NAME psycross_spu_dispatch_test COMMAND psycross_spu_dispatch_test)
endif()
