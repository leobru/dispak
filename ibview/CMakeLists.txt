#
# Add dispak directory to the include path.
#
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../dispak
    ${CMAKE_CURRENT_BINARY_DIR}/../dispak
)

#
# Build 'ibview' binary.
#
add_executable(ibview
    ibview.c
    ../dispak/encoding.c
)
target_compile_options(ibview PRIVATE -Wall -g -O2 -Werror)
install(TARGETS ibview DESTINATION bin)
