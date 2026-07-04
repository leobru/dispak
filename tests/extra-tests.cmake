#
# Custom tests that are not simple output-comparison directories.
# This fragment is appended verbatim to CMakeLists.txt by 'show-tests.py -u'
# (the auto-generated part only covers subdirectories with an options.txt).
# Add hand-written add_test() declarations here so they survive regeneration.
#
add_test(NAME stdin-eof
    COMMAND Python3::Interpreter
        ${CMAKE_SOURCE_DIR}/tests/stdin-eof.py
        $<TARGET_FILE:dispak>
        ${CMAKE_BINARY_DIR}/dispak
        ${CMAKE_SOURCE_DIR}/tests/bemsh-ms/bemsh.b6)

add_test(NAME arfa-e50-id
    COMMAND Python3::Interpreter
        ${CMAKE_SOURCE_DIR}/tests/arfa-e50-id.py
        $<TARGET_FILE:dispak>)
