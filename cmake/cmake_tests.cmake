# NeoBAE compile-path and playbae runtime test helpers.
#
# Run the compile matrix from the repository root with:
#   cmake -P cmake/cmake_tests.cmake
#
# Useful overrides:
#   cmake -DNEOBAE_TEST_PLATFORM=SDL2 -DNEOBAE_TEST_START=5 \
#     -DNEOBAE_TEST_VERBOSE=ON -P cmake/cmake_tests.cmake
#   cmake -DNEOBAE_TEST_CMAKE_ARGS=-DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
#     -P cmake/cmake_tests.cmake

if(CMAKE_SCRIPT_MODE_FILE)
  get_filename_component(NEOBAE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

  set(NEOBAE_TEST_PLATFORM "SDL3" CACHE STRING "Platform used by playbae compile tests")
  set(NEOBAE_TEST_START 1 CACHE STRING "One-based compile test at which to start")
  set(NEOBAE_TEST_JOBS 16 CACHE STRING "Parallel jobs used to build playbae")
  set(NEOBAE_TEST_VERBOSE OFF CACHE BOOL "Show configure and build output")
  set(NEOBAE_TEST_BUILD_ROOT "${NEOBAE_SOURCE_DIR}/build/cmake-tests" CACHE PATH
    "Root directory for compile-test builds")
  set(NEOBAE_TEST_CMAKE_ARGS "" CACHE STRING
    "Additional semicolon-separated arguments passed to each CMake configure")

  set(_neobae_test_number 0)
  set(_neobae_common_args
    -DBAE_PLATFORM=${NEOBAE_TEST_PLATFORM}
    -DBUILD_CLITOOLS=OFF
    -DBUILD_NBSTUDIO=OFF
    -DBUILD_NBEDITOR=OFF
    -DBUILD_RAYLIB_PLAYER=OFF
    -DBUILD_TESTING=OFF
    -DNEOBAE_EXTERNAL_CODECS=OFF
    -DBAE_DISABLE_EMBED_FONT=ON
  )

  function(neobae_run_compile_test test_name)
    math(EXPR _neobae_test_number "${_neobae_test_number} + 1")
    set(_neobae_test_number "${_neobae_test_number}" PARENT_SCOPE)

    message(STATUS "${_neobae_test_number}) Testing ${test_name}")
    if(_neobae_test_number LESS NEOBAE_TEST_START)
      message(STATUS "Skipping test ${_neobae_test_number}")
      return()
    endif()

    string(MAKE_C_IDENTIFIER "${test_name}" _neobae_build_name)
    string(TOLOWER "${_neobae_build_name}" _neobae_build_name)
    set(_neobae_build_dir "${NEOBAE_TEST_BUILD_ROOT}/${_neobae_build_name}")
    file(REMOVE_RECURSE "${_neobae_build_dir}")

    set(_neobae_configure_command
      "${CMAKE_COMMAND}"
      -S "${NEOBAE_SOURCE_DIR}"
      -B "${_neobae_build_dir}"
      ${_neobae_common_args}
      ${NEOBAE_TEST_CMAKE_ARGS}
      ${ARGN}
    )
    if(NEOBAE_TEST_VERBOSE)
      execute_process(COMMAND ${_neobae_configure_command} RESULT_VARIABLE _neobae_result)
    else()
      execute_process(
        COMMAND ${_neobae_configure_command}
        RESULT_VARIABLE _neobae_result
        OUTPUT_VARIABLE _neobae_stdout
        ERROR_VARIABLE _neobae_stderr
      )
    endif()
    if(NOT _neobae_result EQUAL 0)
      message(FATAL_ERROR
        "Configure failed for ${test_name}\n${_neobae_stdout}${_neobae_stderr}")
    endif()

    set(_neobae_build_command
      "${CMAKE_COMMAND}" --build "${_neobae_build_dir}"
      --target playbae --parallel "${NEOBAE_TEST_JOBS}"
    )
    if(NEOBAE_TEST_VERBOSE)
      execute_process(COMMAND ${_neobae_build_command} RESULT_VARIABLE _neobae_result)
    else()
      execute_process(
        COMMAND ${_neobae_build_command}
        RESULT_VARIABLE _neobae_result
        OUTPUT_VARIABLE _neobae_stdout
        ERROR_VARIABLE _neobae_stderr
      )
    endif()
    if(NOT _neobae_result EQUAL 0)
      message(FATAL_ERROR
        "playbae build failed for ${test_name}\n${_neobae_stdout}${_neobae_stderr}")
    endif()
  endfunction()

  # The CMake build enables features by default, so these cases exercise both
  # the full build and each major feature's disabled compile path.
  neobae_run_compile_test(default)
  neobae_run_compile_test(no_sf2 -DBAE_DISABLE_SF2_SUPPORT=ON -DBAE_DISABLE_FLUIDSYNTH=ON)
  neobae_run_compile_test(no_native_dls -DBAE_DISABLE_NATIVE_DLS=ON)
  neobae_run_compile_test(no_native_dls_or_sf2 -DBAE_DISABLE_NATIVE_DLS=ON -DBAE_DISABLE_SF2_SUPPORT=ON -DBAE_DISABLE_FLUIDSYNTH=ON)
  neobae_run_compile_test(no_xmf -DBAE_DISABLE_XMF_SUPPORT=ON)
  neobae_run_compile_test(no_creation_api -DBAE_DISABLE_CREATION_API=ON)
  neobae_run_compile_test(no_zmf -DBAE_DISABLE_ZMF_SUPPORT=ON)
  neobae_run_compile_test(no_rmi -DBAE_DISABLE_RMI_SUPPORT=ON)
  neobae_run_compile_test(no_retro_ringtones -DBAE_DISABLE_RETRO_RINGTONE_SUPPORT=ON)
  neobae_run_compile_test(no_mp3_decoder -DBAE_DISABLE_MP3_DECODER=ON)
  neobae_run_compile_test(no_mp3_encoder -DBAE_DISABLE_MP3_ENCODER=ON)
  neobae_run_compile_test(no_mp3 -DBAE_DISABLE_MP3_ENCODER=ON -DBAE_DISABLE_MP3_DECODER=ON)
  neobae_run_compile_test(no_flac_decoder -DBAE_DISABLE_FLAC_DECODER=ON)
  neobae_run_compile_test(no_flac_encoder -DBAE_DISABLE_FLAC_ENCODER=ON)
  neobae_run_compile_test(no_flac -DBAE_DISABLE_FLAC_ENCODER=ON -DBAE_DISABLE_FLAC_DECODER=ON)
  neobae_run_compile_test(no_vorbis_decoder -DBAE_DISABLE_VORBIS_DECODER=ON)
  neobae_run_compile_test(no_vorbis_encoder -DBAE_DISABLE_VORBIS_ENCODER=ON)
  neobae_run_compile_test(no_vorbis -DBAE_DISABLE_VORBIS_ENCODER=ON -DBAE_DISABLE_VORBIS_DECODER=ON)
  neobae_run_compile_test(no_opus_decoder -DBAE_DISABLE_OPUS_DECODER=ON)
  neobae_run_compile_test(no_opus_encoder -DBAE_DISABLE_OPUS_ENCODER=ON)
  neobae_run_compile_test(no_opus -DBAE_DISABLE_OPUS_ENCODER=ON -DBAE_DISABLE_OPUS_DECODER=ON)
  neobae_run_compile_test(no_qoa -DBAE_DISABLE_QOA_SUPPORT=ON)
  neobae_run_compile_test(no_karaoke -DBAE_DISABLE_KARAOKE=ON)
  neobae_run_compile_test(no_baescript -DBAE_DISABLE_BAESCRIPT=ON)
  neobae_run_compile_test(minimal
    -DBAE_DISABLE_RMF_EDITOR=ON
    -DBAE_DISABLE_SF2_CONVERTER=ON
    -DBAE_DISABLE_ZMF_SUPPORT=ON
    -DBAE_DISABLE_LZMA=ON
    -DBAE_DISABLE_MP3_ENCODER=ON
    -DBAE_DISABLE_MP3_DECODER=ON
    -DBAE_DISABLE_FLAC_ENCODER=ON
    -DBAE_DISABLE_FLAC_DECODER=ON
    -DBAE_DISABLE_VORBIS_ENCODER=ON
    -DBAE_DISABLE_VORBIS_DECODER=ON
    -DBAE_DISABLE_OPUS_ENCODER=ON
    -DBAE_DISABLE_OPUS_DECODER=ON
    -DBAE_DISABLE_KARAOKE=ON
    -DBAE_DISABLE_BAESCRIPT=ON
    -DBAE_DISABLE_PLAYLIST=ON
    -DBAE_DISABLE_SF2_SUPPORT=ON
    -DBAE_DISABLE_FLUIDSYNTH=ON
    -DBAE_DISABLE_NATIVE_DLS=ON
    -DBAE_DISABLE_XMF_SUPPORT=ON
    -DBAE_DISABLE_MTHC_SUPPORT=ON
    -DBAE_DISABLE_ADP_SUPPORT=ON
    -DBAE_DISABLE_ADX_SUPPORT=ON
    -DBAE_DISABLE_RETRO_RINGTONE_SUPPORT=ON
    -DBAE_DISABLE_QOA_SUPPORT=ON
    -DBAE_DISABLE_RMI_SUPPORT=ON
  )

  message(STATUS "All requested playbae compile tests passed")
  return()
endif()

if(NOT TARGET playbae)
  message(STATUS "Skipping playbae tests because the playbae target is unavailable")
  return()
endif()

add_test(NAME playbae_help COMMAND $<TARGET_FILE:playbae> -h)
set_tests_properties(playbae_help PROPERTIES
  PASS_REGULAR_EXPRESSION "USAGE:  playbae"
  TIMEOUT 10
)

# Register a file-loading test using playbae's normal CLI. Extra arguments can
# select a bank, cap playback, or export without using the audio device.
function(neobae_add_playbae_file_test test_name input_file)
  if(NOT IS_ABSOLUTE "${input_file}")
    set(input_file "${CMAKE_CURRENT_SOURCE_DIR}/${input_file}")
  endif()
  if(NOT EXISTS "${input_file}")
    message(FATAL_ERROR "playbae test input does not exist: ${input_file}")
  endif()

  add_test(NAME "${test_name}"
    COMMAND $<TARGET_FILE:playbae> -q -t 1 -f "${input_file}" ${ARGN}
  )
  set_tests_properties("${test_name}" PROPERTIES TIMEOUT 15)
endfunction()

# Runtime test stubs. Enable and tailor these once the desired fixtures and
# backend are available in the test environment.
# neobae_add_playbae_file_test(playbae_load_midi "content/midi/1voice.mid")
# neobae_add_playbae_file_test(playbae_load_rmf "content/rmf/example.rmf")
# neobae_add_playbae_file_test(playbae_load_with_bank "content/midi/1voice.mid"
#   -p "neobae/src/banks/patches111/patches111.hsb")