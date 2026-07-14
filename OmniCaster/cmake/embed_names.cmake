# Converts a Paramdex names .txt into a C byte array header so the DLL can
# print weapon names without shipping loose data files.
#   cmake -DINPUT=<names.txt> -DOUTPUT=<out.inc> -P embed_names.cmake
# Emits: const unsigned char kWeaponNamesTxt[]; const size_t kWeaponNamesTxtLen;
# (A byte array, not a string literal -- MSVC caps literals at 64 KB.)

if(NOT INPUT OR NOT OUTPUT)
    message(FATAL_ERROR "embed_names.cmake needs -DINPUT= and -DOUTPUT=")
endif()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR bytelen "${hexlen} / 2")

# "48656c6c6f" -> "0x48,0x65,..." (wrap lines every 20 bytes for sanity)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){20})" "\\1\n" bytes "${bytes}")

file(WRITE "${OUTPUT}" "// Auto-generated from ${INPUT} -- do not edit.
static const unsigned char kWeaponNamesTxt[] = {
${bytes}
};
static const size_t kWeaponNamesTxtLen = ${bytelen};
")
message(STATUS "embedded ${bytelen} bytes of names -> ${OUTPUT}")
