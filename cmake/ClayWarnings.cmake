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
