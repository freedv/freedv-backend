add_library(fdv_audio_pipeline STATIC
    AgcStep.cpp
    AudioPipeline.cpp
    BandwidthExpandStep.cpp
    EitherOrStep.cpp
    IPipelineStep.cpp
    LevelAdjustStep.cpp
    LinkStep.cpp
    MinimalRealTimeHelper.cpp
    MinimalTxRxThread.cpp
    MuteStep.cpp
    RADEReceiveStep.cpp
    RADETransmitStep.cpp
    ResampleStep.cpp
    RNNoiseStep.cpp
    TapStep.cpp
    ToneInterfererStep.cpp
    rade_text.cpp
    ldpc_encode.cpp
    ldpc_decode.cpp
)

add_dependencies(fdv_audio_pipeline rade opus)
target_compile_options(fdv_audio_pipeline PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(fdv_audio_pipeline PRIVATE ${LIBEBUR128} agc rnnoise fdv_os_wrapper samplerate fdv_util)
target_link_libraries(fdv_audio_pipeline INTERFACE rade opus rnnoise)
target_include_directories(fdv_audio_pipeline PRIVATE ${CODEC2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/..)
target_include_directories(fdv_audio_pipeline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(BUILD_BACKEND_UNITTESTS)
# Real speech from the RADE sources, for tests that need it (see
# test/SpeechSample.h).
ExternalProject_Get_Property(build_rade SOURCE_DIR)
set(RADE_SPEECH_WAV "${SOURCE_DIR}/wav/all.wav")

macro(DefineUnitTest utName)
    add_executable(${utName} test/${utName}.cpp)
    target_link_libraries(${utName} PRIVATE fdv_audio_pipeline fdv_util samplerate)
    target_include_directories(${utName} PRIVATE ${CODEC2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/..)

    add_test(NAME pipeline_${utName} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${utName})
    # Fail a hung test instead of letting it run until CI gives up.
    set_tests_properties(pipeline_${utName} PROPERTIES TIMEOUT 600)

    set_target_properties(
        ${utName}
        PROPERTIES BUILD_RPATH "${CMAKE_BUILD_RPATH};${rade_BINARY_DIR}/src")
endmacro()

DefineUnitTest(AgcStepTest)
DefineUnitTest(AudioPipelineTest)
DefineUnitTest(EitherOrTest)
DefineUnitTest(LevelAdjustTest)
DefineUnitTest(ResampleTest)
DefineUnitTest(TapTest)
DefineUnitTest(BandwidthExpandStepTest)
DefineUnitTest(RNNoiseStepTest)
target_compile_definitions(RNNoiseStepTest PRIVATE SPEECH_WAV_PATH="${RADE_SPEECH_WAV}")
DefineUnitTest(PipelineResetTest)
DefineUnitTest(LinkStepTest)
DefineUnitTest(MuteStepTest)
DefineUnitTest(ToneInterfererTest)

DefineUnitTest(ldpc_decode)
DefineUnitTest(ldpc_encode)

DefineUnitTest(RadeTextTest)
target_link_libraries(RadeTextTest PRIVATE rade opus samplerate)

DefineUnitTest(RadeTextUnitTest)

# Monte Carlo evaluation of rade_text receive combining (not run by ctest;
# see test/RadeTextSim.cpp). Compiles rade_text.cpp directly and always
# optimizes since the simulation is too slow to be useful at -O0.
add_executable(RadeTextSim test/RadeTextSim.cpp rade_text.cpp)
target_compile_options(RadeTextSim PRIVATE -O2)
find_package(Threads REQUIRED)
target_link_libraries(RadeTextSim PRIVATE fdv_util Threads::Threads)
target_include_directories(RadeTextSim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Time-to-decode of rade_text callsigns through the real RADE V2 modem and a
# simulated HF channel (not run by ctest; see test/RadeTextOtaSim.cpp).
# Linked like RadeTextTest: fdv_audio_pipeline must come first, otherwise
# configuring a clean tree fails because the rade IMPORTED target's include
# directory doesn't exist until build_rade has run.
add_executable(RadeTextOtaSim test/RadeTextOtaSim.cpp)
target_compile_options(RadeTextOtaSim PRIVATE -O2)
target_link_libraries(RadeTextOtaSim PRIVATE fdv_audio_pipeline fdv_util rade opus)
target_include_directories(RadeTextOtaSim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
set_target_properties(RadeTextOtaSim PROPERTIES BUILD_RPATH "${CMAKE_BUILD_RPATH};${rade_BINARY_DIR}/src")

DefineUnitTest(RadeStepResetTest)
target_link_libraries(RadeStepResetTest PRIVATE rade opus samplerate)
DefineUnitTest(RadeLoopbackTest)
target_link_libraries(RadeLoopbackTest PRIVATE rade opus samplerate)
target_compile_definitions(RadeLoopbackTest PRIVATE SPEECH_WAV_PATH="${RADE_SPEECH_WAV}")
#endif(LINUX)

# ---------------------------------------------------------------------------
# MinimalTxRxThreadTest – end-to-end TX→RX pipeline quality check via loss.py
#
# Requires Python3 with numpy and torch installed.  Set RADAE_PYTHON_EXECUTABLE
# on the cmake command line to override the auto-detected interpreter, e.g.:
#   cmake -DRADAE_PYTHON_EXECUTABLE=/path/to/venv/bin/python3 ..
# ---------------------------------------------------------------------------
set(RADAE_PYTHON_EXECUTABLE "" CACHE STRING
    "Python3 interpreter (with numpy/torch) for MinimalTxRxThreadTest. Auto-detected if empty.")

if(CMAKE_CROSSCOMPILING)
    # The test runs Python on the build host against the test's output, so it
    # can only run where it's built.
    set(RADAE_PYTHON_EXECUTABLE "")
elseif(NOT RADAE_PYTHON_EXECUTABLE)
    # Try the system Python3 first
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -c "import numpy, torch"
            RESULT_VARIABLE _PYTHON_DEPS_OK
            OUTPUT_QUIET ERROR_QUIET)
        if(_PYTHON_DEPS_OK EQUAL 0)
            set(RADAE_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
        endif()
    endif()
endif()

if(NOT RADAE_PYTHON_EXECUTABLE AND NOT CMAKE_CROSSCOMPILING)
    # Fall back to common virtual-environment locations
    foreach(_PY_CANDIDATE
        "$ENV{HOME}/radae/radae-venv/bin/python3"
        "${CMAKE_BINARY_DIR}/radae-venv/bin/python3"
        "/opt/homebrew/bin/python3"
        "/usr/local/bin/python3")
        if(EXISTS "${_PY_CANDIDATE}")
            execute_process(
                COMMAND "${_PY_CANDIDATE}" -c "import numpy, torch"
                RESULT_VARIABLE _PY_OK
                OUTPUT_QUIET ERROR_QUIET)
            if(_PY_OK EQUAL 0)
                set(RADAE_PYTHON_EXECUTABLE "${_PY_CANDIDATE}")
                break()
            endif()
        endif()
    endforeach()
endif()

DefineUnitTest(MinimalTxRxThreadOversTest)
target_link_libraries(MinimalTxRxThreadOversTest PRIVATE rade opus samplerate)
target_compile_definitions(MinimalTxRxThreadOversTest PRIVATE SPEECH_WAV_PATH="${RADE_SPEECH_WAV}")

# Runs with or without Python; with it, radae's loss.py also scores the
# received speech.
DefineUnitTest(MinimalTxRxThreadTest)
target_link_libraries(MinimalTxRxThreadTest PRIVATE rade opus samplerate)
target_compile_definitions(MinimalTxRxThreadTest PRIVATE SPEECH_WAV_PATH="${RADE_SPEECH_WAV}")

if(RADAE_PYTHON_EXECUTABLE)
    message(STATUS "MinimalTxRxThreadTest: using Python at ${RADAE_PYTHON_EXECUTABLE}")

    # Populate Python RADE scripts. Only loss.py (which compares two feature
    # files) is used, so pin a known-good commit rather than tracking main,
    # where an unrelated change could break the test.
    include(ExternalProject)
    ExternalProject_Add(
        rade_python
	URL https://github.com/drowe67/radae/archive/89220e28fcc25138c0e038849e653dba30ec2dd6.zip
	CONFIGURE_COMMAND ""
	BUILD_COMMAND ""
	INSTALL_COMMAND "")
    ExternalProject_Get_Property(rade_python SOURCE_DIR)

    add_dependencies(MinimalTxRxThreadTest rade_python) # the test runs loss.py from it
    target_compile_definitions(MinimalTxRxThreadTest PRIVATE
	    RADE_SRC_DIR="${SOURCE_DIR}"
        PYTHON_EXECUTABLE="${RADAE_PYTHON_EXECUTABLE}")
else()
    message(STATUS "MinimalTxRxThreadTest: no suitable Python3 found -- loss.py scoring will be skipped")
endif()

# Allow up to 1 hour: 42 rate combinations (RADE NN inference on CPU can be
# significantly slower than real-time).
set_tests_properties(pipeline_MinimalTxRxThreadTest PROPERTIES TIMEOUT 3600)

endif(BUILD_BACKEND_UNITTESTS)
