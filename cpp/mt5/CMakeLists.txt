# The MetaTrader 5 bridge.
#
# Two targets from one source: a static library the tests link against, and the
# DLL MT5 imports. Testing the same code that ships is the point -- a bridge
# whose tested build differs from its deployed build is untested.
add_library(xaumt5 STATIC src/bridge.cpp)
target_include_directories(xaumt5 PUBLIC include)
target_link_libraries(xaumt5 PRIVATE xau_flags)
# PUBLIC: anything linking the static lib must see the same declspec
# decision the library was compiled with, or it asks for __imp_ symbols.
target_compile_definitions(xaumt5 PUBLIC XAU_BRIDGE_STATIC=1)

if(WIN32)
    add_library(xaumt5dll SHARED src/bridge.cpp)
    target_include_directories(xaumt5dll PUBLIC include)
    target_link_libraries(xaumt5dll PRIVATE xau_flags)
    set_target_properties(xaumt5dll PROPERTIES OUTPUT_NAME "xaubridge")
endif()
