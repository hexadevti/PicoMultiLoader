# Gzip a web page into a C header: cmake -DIN=page.html -DOUT=page_html.h -P embed.cmake
# The header defines `index_html` (gzip data, serve with Content-Encoding: gzip) and `index_html_len`.
set(gz "${OUT}.gz")
file(ARCHIVE_CREATE OUTPUT "${gz}" PATHS "${IN}" FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL 9)
file(READ "${gz}" hex HEX)
file(REMOVE "${gz}")
string(LENGTH "${hex}" hex_len)
math(EXPR size "${hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "// Generated from ${IN} - do not edit\n#pragma once\n\n"
     "static const char index_html[] = {\n${bytes}\n};\n"
     "static const unsigned index_html_len = ${size};\n")
