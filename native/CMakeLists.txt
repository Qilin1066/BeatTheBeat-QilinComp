cmake_minimum_required(VERSION 3.20)
project(QilinCompNativeBoot LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "QilinComp requires a 64-bit target")
endif()

set(QILIN_PREPARED_BUILD "${CMAKE_CURRENT_SOURCE_DIR}/../build" CACHE PATH
    "Directory containing the locally generated SOMP01 code and pinned source checkouts")
get_filename_component(prepared "${QILIN_PREPARED_BUILD}" ABSOLUTE)
set(generated "${prepared}/generated/SOMP01_generated")
set(runtime_helpers "${prepared}/mg/vendor/dolphin/GXRuntime")
set(host_headers "${prepared}/mg/include")
set(somp01_sha256 "2e0368359106a2eef773c70ef083e89aafe1e49603eefc7f16b9ed414cd098fe")

foreach(required IN ITEMS "${generated}/main.dol" "${generated}/SOMP01.h"
    "${runtime_helpers}/include/core/cpu.h" "${host_headers}/moderngekko/sha256.hpp")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing ${required}. Run the local disc preparation and AOT generation first.")
    endif()
endforeach()
file(SHA256 "${generated}/main.dol" dol_sha256)
if(NOT dol_sha256 STREQUAL somp01_sha256)
    message(FATAL_ERROR "The local translation must use the verified PAL SOMP01 revision 0 executable")
endif()

file(GLOB chunks CONFIGURE_DEPENDS "${generated}/chunks/*.c")
list(LENGTH chunks chunk_count)
if(NOT chunk_count EQUAL 126)
    message(FATAL_ERROR "Expected 126 AOT source chunks from the pinned DolRecomp build")
endif()

foreach(chunk IN LISTS chunks)
    get_filename_component(name "${chunk}" NAME_WE)
    set(wrapper "${CMAKE_CURRENT_BINARY_DIR}/chunks/${name}.cpp")
    file(GENERATE OUTPUT "${wrapper}"
        CONTENT "#include <math.h>
#include <string.h>
#include \"aot_cache.hpp\"
#define ppc_fallback_instruction(cpu, instruction, pc) qilin::lower_generated_operation<instruction, pc>(cpu)
extern \"C\" {
#include \"${chunk}\"
}
#undef ppc_fallback_instruction
")
    list(APPEND translated_sources "${wrapper}")
endforeach()

# These files provide fixed arithmetic operations, FP estimate tables and state helpers.
set(helpers
    "${runtime_helpers}/src/core/cpu.c"
    "${runtime_helpers}/src/core/cpu_exception.c"
    "${runtime_helpers}/src/core/cpu_interpreter_float.c"
    "${runtime_helpers}/src/core/cpu_interpreter_table.c")
set_source_files_properties(${helpers} PROPERTIES LANGUAGE CXX)
add_executable(qilin-native-boot boot_probe.cpp ${helpers} ${translated_sources})
set_target_properties(qilin-native-boot PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(qilin-native-boot PRIVATE "${generated}" "${CMAKE_CURRENT_SOURCE_DIR}"
    "${runtime_helpers}/include" "${host_headers}")
target_compile_definitions(qilin-native-boot PRIVATE DOLRECOMP_CPU_HEADER="core/cpu.h"
    QILIN_SOMP01_SHA256="${somp01_sha256}")
if(MSVC)
    target_compile_options(qilin-native-boot PRIVATE /fp:strict)
else()
    target_compile_options(qilin-native-boot PRIVATE -ffp-contract=off -fno-fast-math)
endif()
