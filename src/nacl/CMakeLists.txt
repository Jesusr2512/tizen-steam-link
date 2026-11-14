add_library(h264bitstream STATIC
    h264bitstream/h264_nal.c
    h264bitstream/h264_sei.c
    h264bitstream/h264_stream.c
)
target_include_directories(h264bitstream PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/h264bitstream)
target_compile_options(h264bitstream PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare)

add_executable(tsl_nacl
    app.cpp
    player.cpp
    udp_node_override.cpp
)

set_target_properties(tsl_nacl PROPERTIES
    OUTPUT_NAME "tsl_unfinalized"
    SUFFIX ".bc"
)

target_include_directories(tsl_nacl PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${NACL_INCLUDE_DIRS}
)

target_link_libraries(tsl_nacl PRIVATE
    tsl-core
    opus
    h264bitstream
    ppapi_gles2
    ppapi_cpp
    ppapi
    nacl_io
    pthread
)

add_custom_command(TARGET tsl_nacl POST_BUILD
    COMMAND ${PNACL_FINALIZE} $<TARGET_FILE:tsl_nacl> -o ${CMAKE_CURRENT_BINARY_DIR}/tsl.pexe
    COMMAND ${PNACL_TRANSLATE} -arch arm -O3 -mattr=+neon ${CMAKE_CURRENT_BINARY_DIR}/tsl.pexe -o ${CMAKE_SOURCE_DIR}/web/tsl-arm.nexe
    COMMAND ${PNACL_TRANSLATE} -arch x86-32 -O3 ${CMAKE_CURRENT_BINARY_DIR}/tsl.pexe -o ${CMAKE_SOURCE_DIR}/web/tsl-x86-32.nexe
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/tsl.nmf ${CMAKE_SOURCE_DIR}/web/tsl.nmf
    COMMENT "Finalizing and translating PNaCl bitcode to tsl-arm.nexe and tsl-x86-32.nexe"
)
