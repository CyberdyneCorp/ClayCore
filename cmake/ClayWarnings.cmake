# Warning and sanitizer configuration shared by all claycore targets.

function(clay_apply_warnings target)
  if(MSVC)
    # C4723 fires on guarded divisions (slab tests); the guards are real.
    # _CRT_SECURE_NO_WARNINGS: standard fopen/sscanf are used portably.
    target_compile_options(${target} PRIVATE /W4 /permissive- /wd4723)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(CLAY_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    # CXX-only: nvcc rejects host warning flags on .cu sources (it parses
    # -Werror as its own option). CUDA device code is gated by the CUDA CI
    # job and the kernel-dialect check's CUDA profile instead.
    target_compile_options(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Wshadow>)
    # GCC 12 and 13 report -Wstringop-overflow and -Warray-bounds from inside
    # libstdc++'s stl_algobase.h when std::vector operations are inlined at
    # -O3. They are long-standing false positives in the header, not in our
    # code, and they only appear on the manylinux_2_28 toolchain the release
    # wheels use. Demote them rather than silence them, so the diagnostic
    # stays visible if it ever points somewhere real.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
      target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=stringop-overflow -Wno-error=array-bounds>)
    endif()
    if(CLAY_WERROR)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
    endif()
  endif()
endfunction()

function(clay_apply_sanitizers target)
  if(CLAY_SANITIZE AND NOT MSVC)
    set(_clay_san -fsanitize=${CLAY_SANITIZE} -fno-omit-frame-pointer -g)
    if(CLAY_SANITIZE MATCHES "undefined")
      # UBSan prints and continues by default, so a sanitizer build could
      # report undefined behaviour and still pass ctest — the finding lands in
      # a log nobody reads. Make it fatal, which is only possible once vptr is
      # off: that check needs RTTI and claycore builds -fno-rtti, so every
      # polymorphic call through a backend or a shared_ptr control block is a
      # false positive.
      list(APPEND _clay_san -fno-sanitize=vptr -fno-sanitize-recover=undefined)
    endif()
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${_clay_san}>)
    target_link_options(${target} PRIVATE -fsanitize=${CLAY_SANITIZE})
  endif()
endfunction()
