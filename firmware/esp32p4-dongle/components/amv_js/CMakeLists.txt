set(qjs_sources "quickjs-ng/quickjs.c" "quickjs-ng/libregexp.c" "quickjs-ng/libunicode.c" "quickjs-ng/dtoa.c")

idf_component_register(SRCS "amv_js.c" ${qjs_sources}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "quickjs-ng"
                       PRIV_REQUIRES esp_timer esp_hw_support heap freertos pthread
                       EMBED_FILES "generated/openmouse.bc")

target_compile_definitions(${COMPONENT_LIB} PRIVATE _GNU_SOURCE)
# The engine is vendored upstream code (quickjs-ng/VENDORED.txt): built fast, its warnings are not ours.
set_source_files_properties(${qjs_sources} PROPERTIES COMPILE_OPTIONS "-O2;-w")
