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
    if(CLAY_WERROR)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
    endif()
  endif()
endfunction()

function(clay_apply_sanitizers target)
  if(CLAY_SANITIZE AND NOT MSVC)
    target_compile_options(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=${CLAY_SANITIZE} -fno-omit-frame-pointer -g>)
    target_link_options(${target} PRIVATE -fsanitize=${CLAY_SANITIZE})
  endif()
endfunction()
