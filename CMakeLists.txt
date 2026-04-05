add_library(neobae STATIC
  neobae/src/BAE_Source/Platform/BAE_API_Dummy.c
  neobae/src/BAE_Source/Common/DriverTools.c
  neobae/src/BAE_Source/Common/GenAudioStreams.c
  neobae/src/BAE_Source/Common/GenCache.c
  neobae/src/BAE_Source/Common/GenChorus.c
  neobae/src/BAE_Source/Common/GenFiltersReverbU3232.c
  neobae/src/BAE_Source/Common/GenInterp2ReverbU3232.c
  neobae/src/BAE_Source/Common/GenOutput.c
  neobae/src/BAE_Source/Common/GenPatch.c
  neobae/src/BAE_Source/Common/GenReverb.c
  neobae/src/BAE_Source/Common/GenReverbNew.c
  neobae/src/BAE_Source/Common/GenReverbNeo.c
  neobae/src/BAE_Source/Common/GenRMI.c
  neobae/src/BAE_Source/Common/GenSample.c
  neobae/src/BAE_Source/Common/GenSeq.c
  neobae/src/BAE_Source/Common/GenSeqTools.c
  neobae/src/BAE_Source/Common/GenSetup.c
  neobae/src/BAE_Source/Common/GenSong.c
  neobae/src/BAE_Source/Common/GenSoundFiles.c
  neobae/src/BAE_Source/Common/GenSynth.c
  neobae/src/BAE_Source/Common/GenSynthFiltersSimple.c
  neobae/src/BAE_Source/Common/GenSynthFiltersU3232.c
  neobae/src/BAE_Source/Common/GenSynthInterp2Simple.c
  neobae/src/BAE_Source/Common/GenSynthInterp2U3232.c
  neobae/src/BAE_Source/Common/NeoBAE.c
  neobae/src/BAE_Source/Common/NewNewLZSS.c
  neobae/src/BAE_Source/Common/SampleTools.c
  neobae/src/BAE_Source/Common/X_API.c
  neobae/src/BAE_Source/Common/X_Decompress.c
  neobae/src/BAE_Source/Common/X_IMA.c
  neobae/src/BAE_Source/Common/X_LZMA.c
  neobae/src/BAE_Source/Common/g711.c
  neobae/src/BAE_Source/Common/g721.c
  neobae/src/BAE_Source/Common/g723_24.c
  neobae/src/BAE_Source/Common/g723_40.c
  neobae/src/BAE_Source/Common/g72x.c
  neobae/src/BAE_Source/Common/sha1mini.c
  neobae/src/BAE_Source/Common/XFileTypes.c
  neobae/src/BAE_Source/Common/BAERmfEditor.c
  neobae/src/sf2-hsb/hsb_writer.c
  neobae/src/sf2-hsb/sf2_hsb_converter.c
  neobae/src/sf2-hsb/sf2_parser.c
)

target_include_directories(neobae
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/BAE_Source/Common
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/BAE_Source/Platform
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/sf2-hsb
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/thirdparty/config
)

target_compile_definitions(neobae
  PUBLIC
    X_PLATFORM=X_DUMMY
    USE_CREATION_API=1
    USE_ZMF_SUPPORT=1
    USE_LZMA_COMPRESSION=1
)

target_compile_options(neobae PRIVATE -w)
target_compile_features(neobae PUBLIC c_std_99)

find_package(Threads REQUIRED)
target_link_libraries(neobae PUBLIC Threads::Threads)

if(UNIX AND NOT APPLE)
  target_link_libraries(neobae PUBLIC m)
  if(CMAKE_DL_LIBS)
    target_link_libraries(neobae PUBLIC ${CMAKE_DL_LIBS})
  endif()
endif()

find_library(LIBLZMA_LIBRARY NAMES liblzma lzma)
find_path(LIBLZMA_INCLUDE_DIR lzma.h)
if(LIBLZMA_LIBRARY)
  if(LIBLZMA_INCLUDE_DIR)
    target_include_directories(neobae PRIVATE ${LIBLZMA_INCLUDE_DIR})
  endif()
  if(WIN32)
    target_compile_definitions(neobae PRIVATE LZMA_API_STATIC)
  endif()
  target_link_libraries(neobae PUBLIC ${LIBLZMA_LIBRARY})
endif()
