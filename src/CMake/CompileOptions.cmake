cmake_minimum_required(VERSION 3.26.0 FATAL_ERROR)

# Optional compile flags

# Use pure -std=c++XX, instead -std=gnu++XX
set(CMAKE_CXX_EXTENSIONS OFF)

# C++20 Modules
# https://gitlab.kitware.com/cmake/cmake/-/blob/v3.27.0/Help/dev/experimental.rst
if (WITH_MODULES)
  # TODO: Remove in CMake 3.28
  set(CMAKE_EXPERIMENTAL_CXX_MODULE_CMAKE_API "aa1f7df0-828a-4fcd-9afc-2dc80491aca7")
  set(CMAKE_EXPERIMENTAL_CXX_MODULE_DYNDEP ON)
  # TODO: pch and c++20 modules error : dependency cycle
  set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
  if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  endif()
endif()

# Unit tests
# https://github.com/google/googletest
if (WITH_TESTS)
  # Enable CTest
  enable_testing()
  set(CXX_TEST_FLAG "-D_TEST")
endif()

# Force Synchronous PDB Writes
if (WIN32)
  set(CXX_PDB_SYNC "/FS")
endif()

# Generated PCH headers
if (NOT WITH_PCH)
  set(CXX_GENERATED_PCH_FLAG "-DDONT_USE_GENERATED_PCH")
endif()

# AddressSanitizer (ASan)
# https://devblogs.microsoft.com/cppblog/address-sanitizer-for-msvc-now-generally-available/
if (WITH_ASAN)
  set(CXX_ASAN_FLAG "-fsanitize=address")
  # msvc 2022 "mismatch detected for 'annotate_vector'" workaround
  # https://learn.microsoft.com/en-us/answers/questions/864574/enabling-address-sanitizer-results-in-error-lnk203
  if (WIN32)
    set(CXX_ASAN_FLAG "${CXX_ASAN_FLAG};-D_DISABLE_VECTOR_ANNOTATION")
  endif()
    # default `-fsanitize=address` incompatible with `--no-undefined`. use -shared-libsan
  if (UNIX)
    set(CXX_ASAN_FLAG "${CXX_ASAN_FLAG};-fno-omit-frame-pointer")
    set(LINK_ASAN_FLAG "-fsanitize=address;-shared-libsan")

    # TODO: ASAN and boost::dll::load_mode::search_system_folders
    # https://bugs.llvm.org/show_bug.cgi?id=27790
    set(LINK_ASAN_FLAG "${LINK_ASAN_FLAG};-Wl,--disable-new-dtags")
  endif()
endif()

# mold: very fast linker for linux
# https://github.com/rui314/mold#mold-a-modern-linker
if (WITH_MOLD)
  find_program(MOLD_PATH mold REQUIRED)
endif()

# CodeCover
# https://clang.llvm.org/docs/SanitizerCoverage.html#instrumentation-points
if (WITH_CODECOVER)
  set(CXX_CODECOVER_FLAG 
    # Clang & MSVC 2022 https://docs.microsoft.com/en-us/cpp/build/reference/fsanitize-coverage?view=msvc-170
    "-fsanitize-coverage=func"            # analyse only functions              https://clang.llvm.org/docs/SanitizerCoverage.html#instrumentation-points
    "-fsanitize-coverage=trace-pc-guard"  # use uint32 counters                 https://clang.llvm.org/docs/SanitizerCoverage.html#tracing-pcs-with-guards
    # "-fsanitize-coverage=no-prune"      # do not eliminate edges

    "-fsanitize-coverage-allowlist=${CMAKE_CURRENT_SOURCE_DIR}/allowlist.txt"  # sanitize only our sources
    "-fsanitize-coverage-ignorelist=${CMAKE_CURRENT_SOURCE_DIR}/blocklist.txt" # skip codeceover.impl itself

    "-DWITH_CODECOVER"
  )
endif()

# Compile flags

# all c++ projects compile flags
set(CMAKE_CXX_FLAGS "${CXX_TEST_FLAG} ${CXX_GENERATED_PCH_FLAG} ${CXX_CLANG_FLAGS} ${CXX_PDB_SYNC}")
set(CMAKE_C_FLAGS "")

# Windows specific flags
if(WIN32)
  
  # ClangCL workaround
  if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CXX_INLINE_LEVEL "/Ob0")
    # fatal error : UTF-16 (LE) byte order mark detected
    set(CMAKE_NINJA_CMCLDEPS_RC OFF)
  else()
    set(CXX_INLINE_LEVEL "/Ob1")
  endif()

  set(CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO} /OPT:ICF /OPT:REF /INCREMENTAL:NO")

  set(CMAKE_CXX_FLAGS_DEBUG "/Ob0 /Od")
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  # Release
  set(COMMON_RELEASE_FLAGS "/Oi /Ot /GF /DNDEBUG")
  set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 ${COMMON_RELEASE_FLAGS}")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/O2 ${CXX_INLINE_LEVEL} /Gy ${COMMON_RELEASE_FLAGS}")
  set(CMAKE_CXX_FLAGS_MINSIZEREL "/O1 /Ob1 ${COMMON_RELEASE_FLAGS}")
  set(CMAKE_C_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
  set(CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL}")
  # RC
  set(CMAKE_RC_FLAGS "/nologo /D_UNICODE /DUNICODE /D_AFXDLL")
  set(CMAKE_RC_FLAGS_DEBUG "/D_TEST /D_DEBUG")
  set(CMAKE_RC_FLAGS_RELEASE "/DNDEBUG")
  set(CMAKE_RC_FLAGS_RELWITHDEBINFO "/DNDEBUG")
  set(CMAKE_RC_FLAGS_MINSIZEREL "/DNDEBUG")

  # MIDL
  set(MIDL_FLAGS "/DNDEBUG")
endif()

# Toolset
set(PLATFORM ${CMAKE_GENERATOR_PLATFORM})

# UI flags
set(UI_LINKER_FLAGS
  "$<$<CONFIG:Debug>:/ASSEMBLYDEBUG>"
  "$<$<CONFIG:Release,RelWithDebInfo>:/OPT:ICF>"
  "$<$<CONFIG:Release,RelWithDebInfo>:/OPT:REF>"
)
set(UI_COMPILE_FLAGS "$<$<CONFIG:Release,RelWithDebInfo>:/GL>")

# Platform x64/x86
if (${PLATFORM} STREQUAL "x64")
  set(PLATFORM_PREFIX "x")
  set(PLATFORM_POSTFIX "64")
  set(CMAKE_RC_FLAGS "/D_WIN64 ${CMAKE_RC_FLAGS}")
else()
  set(PLATFORM_PREFIX "")
  set(PLATFORM_POSTFIX "")
  set(CMAKE_RC_FLAGS "/D_WIN32 ${CMAKE_RC_FLAGS}")
endif()

if (${CMAKE_BUILD_TYPE} STREQUAL "Debug")
  set(CONFIG "Debug")
  set(POSTFIX "d")
else()
  set(CONFIG "Release")
  set(POSTFIX "")
endif()

# System specific dir for implementation
if(WIN32)
  set(SYSDIR "Windows")
else()
  set(SYSDIR "Posix")
endif()

set(CXX_STD_VERSION
  "$<IF:$<AND:$<BOOL:${WIN32}>,$<CXX_COMPILER_ID:Clang>>,cxx_std_23,cxx_std_20>" # Clang && Windows C++23
)

# projects without /clr (not ui)
set(ADDITIONAL_COMPILE_OPTIONS_CXX
  "$<$<BOOL:${UNIX}>:-fms-extensions>" # TODO: msvc extensions support (__super, ...)

  # TODO: dynamic_cast problem
  # "$<$<BOOL:${UNIX}>:-fvisibility=hidden>" # hide all symbols by default
  "$<$<BOOL:${UNIX}>:-fvisibility-ms-compat>" # https://clang.llvm.org/docs/ClangCommandLineReference.html#cmdoption-clang-fvisibility-ms-compat
  
  "$<$<BOOL:${UNIX}>:-fexperimental-library>" # TODO: make `join_view` non-experimental once D2770 is implemented.
  "$<$<BOOL:${UNIX}>:$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>>" # use clang stdlib implementation
  # https://reviews.llvm.org/D60480
  # "$<$<BOOL:${UNIX}>:-D_LIBCPP_HAS_PARALLEL_ALGORITHMS>"
  # https://releases.llvm.org/16.0.0/projects/libcxx/docs/ReleaseNotes.html#deprecations-and-removals
  "$<$<BOOL:${UNIX}>:-D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES>"
  # ClangCL: allow nullptr dereference https://clang.llvm.org/docs/ClangCommandLineReference.html#cmdoption-clang-fdelete-null-pointer-checks
  "$<$<BOOL:${WIN32}>:$<$<CXX_COMPILER_ID:Clang>:-fno-delete-null-pointer-checks>>" # TODO: for support mfc on windows
  # explicit "_DEBUG" for clang (linux)
  "$<$<CONFIG:Debug>:-D_DEBUG>"
  # https://aras-p.info/blog/2019/01/16/time-trace-timeline-flame-chart-profiler-for-Clang/
  "$<$<BOOL:${WITH_TIMETRACE}>:$<$<CXX_COMPILER_ID:Clang>:-ftime-trace>>"
  # faster GDB startup
  "$<$<BOOL:${UNIX}>:$<$<BOOL:${WITH_LLD}>:-gsplit-dwarf>>"
  "$<$<BOOL:${UNIX}>:$<$<BOOL:${WITH_LLD}>:-gdwarf-5>>"
  "${CXX_CODECOVER_FLAG}"
  "${CXX_ASAN_FLAG}"
)

# linker flags
set(ADDITIONAL_CXX_LINKER_FLAGS
  "$<$<BOOL:${UNIX}>:-stdlib=libc++>"
  "$<$<BOOL:${UNIX}>:-Wl,--no-undefined>"
  "$<$<BOOL:${UNIX}>:$<$<BOOL:${WITH_MOLD}>:-fuse-ld=${MOLD_PATH}>>"
  "$<$<BOOL:${UNIX}>:-Wl,--exclude-libs,ALL>"
  "$<$<BOOL:${WIN32}>:$<$<BOOL:${WITH_CODECOVER}>:/PDBALTPATH:%_PDB%>>"
  "$<$<BOOL:${UNIX}>:-latomic>"
  "$<$<BOOL:${UNIX}>:$<$<BOOL:${WITH_ASAN}>:${LINK_ASAN_FLAG}>>"
  # faster GDB startup
  "$<$<BOOL:${UNIX}>:$<$<BOOL:${WITH_LLD}>:-fuse-ld=lld>>"
  "$<$<BOOL:${UNIX}>:$<$<BOOL:${WITH_LLD}>:-Wl,--gdb-index>>"
)

# target link libraries
set(ADDITIONAL_CXX_LINK_LIBRARIES
  "$<$<BOOL:${WITH_CODECOVER}>:CodeCover>"
  "$<$<BOOL:${UNIX}>:${CMAKE_DL_LIBS}>"
)

# Warnings
set(WARNINGS_AS_ERROR
  "$<$<CXX_COMPILER_ID:MSVC>:/WX>"
)

# TODO: all clang warnings
set(CLANG_WARNINGS
  # disabled
  "-Werror"
  "-Wno-#pragma-messages"
  "-Wno-bitfield-constant-conversion"
  "-Wno-constant-conversion"
  "-Wno-defaulted-function-deleted"
  "-Wno-deprecated-anon-enum-enum-conversion"
  "-Wno-deprecated-declarations"
  "-Wno-deprecated-enum-compare"
  "-Wno-deprecated-enum-compare-conditional"
  "-Wno-deprecated-enum-enum-conversion"
  "-Wno-deprecated-enum-float-conversion"
  "-Wno-deprecated-volatile"
  "-Wno-dynamic-class-memaccess"
  "-Wno-enum-compare-switch"
  "-Wno-extra-tokens"
  "-Wno-ignored-attributes"
  "-Wno-ignored-reference-qualifiers"
  "-Wno-implicit-const-int-float-conversion"
  "-Wno-int-to-pointer-cast"
  "-Wno-int-to-void-pointer-cast"
  "-Wno-logical-not-parentheses"
  "-Wno-microsoft-cast"
  "-Wno-microsoft-template-shadow"
  "-Wno-missing-declarations"
  "-Wno-multichar"
  "-Wno-non-literal-null-conversion"
  "-Wno-parentheses"
  "-Wno-parentheses-equality"
  "-Wno-pointer-bool-conversion"
  "-Wno-pointer-to-int-cast"
  "-Wno-potentially-evaluated-expression"
  "-Wno-reinterpret-base-class"
  "-Wno-return-type-c-linkage"
  "-Wno-shift-negative-value"
  "-Wno-sizeof-pointer-memaccess"
  "-Wno-switch"
  "-Wno-undefined-bool-conversion"
  "-Wno-unused-comparison"
  "-Wno-unused-value"
  "-Wno-deprecated-builtins"
  # enabled
  "-Wdelete-non-virtual-dtor"
  "-Wmove"
  "-Warray-parameter"
  "-Wframe-address"
  "-Wimplicit"
  "-Winfinite-recursion"
  "-Wint-in-bool-context"
  "-Wmismatched-tags"
  "-Wmissing-braces"
  "-Wrange-loop-construct"
  "-Wself-assign"
  # TODO: "-Wundefined-reinterpret-cast"
  # TODO: "-Wweak-vtables"
  "-Wloop-analysis"
  "-Wunreachable-code"
  "-Wunused-function"
  "-Wold-style-cast"
  "-Wgnu"
  # TODO: "-Wextra-semi"
)

# clang on windows requires "/clang:" prefix
if (WIN32)
  list(TRANSFORM CLANG_WARNINGS PREPEND "/clang:")
endif()

# Disable warnings for legacy projects only!
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  # custom clang warnings
  set(DISABLE_CLANG_WARNINGS_FOR_LEGACY_PROJECT
    "-Wno-old-style-cast"
  )
  # clang on windows requires "/clang:" prefix
  if (WIN32)
    list(TRANSFORM DISABLE_CLANG_WARNINGS_FOR_LEGACY_PROJECT PREPEND "/clang:")
  endif()
endif()

set(WARNINGS_LEVEL
  "$<$<CXX_COMPILER_ID:MSVC>:/W3>"
)

set(DISABLED_WARNINGS
  "$<$<BOOL:${WIN32}>:/wd4018>"
  "$<$<BOOL:${WIN32}>:/wd4251>"
  "$<$<BOOL:${WIN32}>:/wd4275>"
)

set(ELIMINATE_DUPLICATE_STRINGS "/GF")
set(OPENMP "$<$<CXX_COMPILER_ID:MSVC>:/openmp>")
set(ASYNC_EXCEPTION_HANDLING_MODEL "/EHa")
set(EXCEPTION_HANDLING_MODEL "$<$<BOOL:${WIN32}>:/EHsc>")
set(DEBUG_INFORMATION_FORMAT "$<$<BOOL:${WIN32}>:/Zi>")
set(MULTIPROCESSORCOMPILATION "$<$<CXX_COMPILER_ID:MSVC>:/MP>")
set(SUBSYSTEM "$<$<BOOL:${WIN32}>:/SUBSYSTEM:WINDOWS>")
set(STACK_SIZE "$<$<BOOL:${WIN32}>:/STACK:2000000,2000000>")
set(CONSTEXPR_LIMIT "10000000")
set(CONSTEXPR_STEPS "$<$<CXX_COMPILER_ID:MSVC>:/constexpr:steps${CONSTEXPR_LIMIT}>" "$<$<BOOL:${WIN32}>:$<$<CXX_COMPILER_ID:Clang>:/clang:-fconstexpr-steps=${CONSTEXPR_LIMIT}>>" "$<$<BOOL:${UNIX}>:-fconstexpr-steps=${CONSTEXPR_LIMIT}>")
set(DOTNET47_PATH "$<$<NOT:$<BOOL:${MSVC_IDE}>>:/AIC:/Program Files (x86)/Reference Assemblies/Microsoft/Framework/.NETFramework/v4.7/>")

set(CONFORMANCE_COMPILE_OPTIONS
  # enforce standard c++
  # https://docs.microsoft.com/en-us/cpp/build/reference/permissive-standards-conformance?view=msvc-170
  "$<$<CXX_COMPILER_ID:MSVC>:/permissive->"
  # /permissive- mode for third party libs (c3d)
  "$<$<BOOL:${WIN32}>:/D_MSVC_PERMISSIVE_OFF>"
  # "$<$<CXX_COMPILER_ID:MSVC>:/D_MSVC_PERMISSIVE_OFF>"
)

set(COMMON_COMPILE_OPTIONS
  "$<$<CXX_COMPILER_ID:Clang>:${CLANG_WARNINGS}>"
  ${ADDITIONAL_COMPILE_OPTIONS_CXX}
  ${MULTIPROCESSORCOMPILATION}
  ${CONFORMANCE_COMPILE_OPTIONS}
  ${CONSTEXPR_STEPS}
  ${DISABLED_WARNINGS}
  ${DEBUG_INFORMATION_FORMAT}
  ${EXCEPTION_HANDLING_MODEL}
  ${WARNINGS_AS_ERROR}
  ${WARNINGS_LEVEL}
)

set(AFXDLL "_AFXDLL")
set(AFXEXT "_AFXEXT")
set(CONSOLE "_CONSOLE")

set(WINDOWS_MACROS
  "WIN32"
  "_WINDOWS"
  "_WIN32_WINNT=0x0600"
  "WINVER=0x0600"
  "NOMINMAX"
  # Exclude rarely-used stuff from Windows headers
  "VC_EXTRALEAN" 
  "WIN32_LEAN_AND_MEAN"
)

set(UNICODE_MACROS
  "UNICODE"
  "_UNICODE"
  "RPC_USE_NATIVE_WCHAR"
)

set(BOOST_MACROS
  # https://www.boost.org/doc/libs/1_78_0/doc/html/BOOST_DLL_USE_STD_FS.html
  # Define this macro to make Boost.DLL use C++17's std::filesystem::path, std::system_error and std::error_code.
  "BOOST_DLL_USE_STD_FS"
  "$<$<BOOL:${WITH_ASAN}>:BOOST_USE_ASAN>"
)

# TODO: remove
set(DEPRECATION_MACROS
  "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING"
)

set(PROTECT_SOFT
  # ProtDefs.h
  "_PROTECT_SOFT"
  "_USE_SP_DSS=0"
)

set(CRT_WARNING_MACROS
  "_CRT_SECURE_NO_DEPRECATE"
  "_CRT_SECURE_NO_WARNINGS"
)

set(RENDER_MACROS
  "ENABLE_TEXTURE"
  "GLEW_MX"
)

set(DEBUG_MACROS
  "_SECURE_SCL=0"
)

set(DEPRECATED_AFXWIN_USAGE
  "$<$<BOOL:${WIN32}>:/FI${CMAKE_SOURCE_DIR}/MFCUtils/MFCUtils/AfxCompat.h>" # TODO: WinApi
)

set(DEPRECATED_BCG_USAGE
  "$<$<BOOL:${WIN32}>:/FI${CMAKE_SOURCE_DIR}/BCG/BCG/BCGIncludeFull.h>" # TODO: WinApi
)

set(FEATURE_MACROS
  "$<$<BOOL:${WITH_LINUX_VERSION}>:WITH_LINUX_VERSION>"
  "$<$<BOOL:${PURE_QT}>:PURE_QT>"
  "$<$<BOOL:${WITH_WINDOWS_API}>:WITH_WINDOWS_API>"
  "$<$<BOOL:${WITH_CROSSPLATFORM_API}>:WITH_CROSSPLATFORM_API>"

  # 3D 03100_obj3D_macros.h
  "_EDITING_FACESFILL"
  "USE_UNIFIED_PRIM_RESET"
  "MIX_STORE_FILE_VERSION=0x0A00000BL"
)

set(COMMON_MACROS
  ${FEATURE_MACROS}
  ${RENDER_MACROS}
  ${DEBUG_MACROS}
  ${CRT_WARNING_MACROS}
  ${UNICODE_MACROS}
  "$<$<BOOL:${WIN32}>:${WINDOWS_MACROS}>"
  ${BOOST_MACROS}
)