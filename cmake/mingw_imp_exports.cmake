# Writes a .def that exports every __imp_* symbol the given objects define.
# MinGW only: lld's auto-export (MinGW's WINDOWS_EXPORT_ALL_SYMBOLS) leaves
# these out, but the recompiler names a title's kernel imports __imp__<name>.
#   cmake -DNM=<llvm-nm> -DRSP=<file listing the objects> -DOUT=<def> -P mingw_imp_exports.cmake
# $<TARGET_OBJECTS> also lists the precompiled header (.pch): keep the objects.
file(STRINGS "${RSP}" _lines)
list(FILTER _lines INCLUDE REGEX "\\.(obj|o)\"?$")
list(JOIN _lines "\n" _objects)
file(WRITE "${OUT}.rsp" "${_objects}\n")
execute_process(
    COMMAND "${NM}" --extern-only --defined-only --just-symbol-name "@${OUT}.rsp"
    OUTPUT_VARIABLE _symbols
    RESULT_VARIABLE _result)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "llvm-nm failed (${_result})")
endif()
string(REGEX MATCHALL "(^|\n)__imp_[A-Za-z0-9_@$?]+" _imports "${_symbols}")
list(TRANSFORM _imports STRIP)
list(REMOVE_DUPLICATES _imports)
list(SORT _imports)
list(JOIN _imports "\n    " _body)
file(WRITE "${OUT}.tmp" "EXPORTS\n    ${_body}\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
list(LENGTH _imports _count)
message(STATUS "rexruntime: ${_count} __imp_ exports for MinGW")
