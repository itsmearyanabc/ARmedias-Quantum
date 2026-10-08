# The MetaTrader 5 bridge.
#
# Two targets from one source: a static library the tests link against, and the
# DLL MT5 imports. Testing the same code that ships is the point -- a bridge
# whose tested build differs from its deployed build is untested.
add_library(xaumt5 STATIC src/bridge.cpp)
target_include_directories(xaumt5 PUBLIC include)
# xau::core is what makes the bridge trade the tested strategy: LiveSession,
# the registry and the risk layer. It has no dependencies of its own, which is
# why it can go inside a DLL that MetaTrader loads.
target_link_libraries(xaumt5 PRIVATE xau::core xau_flags)
# PUBLIC: anything linking the static lib must see the same declspec
# decision the library was compiled with, or it asks for __imp_ symbols.
target_compile_definitions(xaumt5 PUBLIC XAU_BRIDGE_STATIC=1)

if(WIN32)
    # The DLL carries its own C runtime. MetaTrader runs on machines that may
    # not have the Visual C++ redistributable, and a missing VCRUNTIME140.dll
    # fails the EA's #import with an error naming neither. Static CRT for the
    # DLL means static CRT for everything linked into it -- MSVC refuses to mix
    # runtimes (LNK2038) -- so it gets its own build of the core, from the same
    # sources the tests run.
    get_target_property(_core_src xaucore SOURCES)
    get_target_property(_core_dir xaucore SOURCE_DIR)
    list(TRANSFORM _core_src PREPEND "${_core_dir}/")
    set(_static_crt "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    add_library(xaucore_static_crt STATIC ${_core_src})
    target_include_directories(xaucore_static_crt PUBLIC ${_core_dir}/include)
    target_link_libraries(xaucore_static_crt PRIVATE xau_flags)
    set_target_properties(xaucore_static_crt PROPERTIES
        MSVC_RUNTIME_LIBRARY "${_static_crt}"
        POSITION_INDEPENDENT_CODE ON)

    add_library(xaumt5dll SHARED src/bridge.cpp)
    target_include_directories(xaumt5dll PUBLIC include)
    target_link_libraries(xaumt5dll PRIVATE xaucore_static_crt xau_flags)
    set_target_properties(xaumt5dll PROPERTIES
        OUTPUT_NAME "xaubridge"
        MSVC_RUNTIME_LIBRARY "${_static_crt}")
endif()
