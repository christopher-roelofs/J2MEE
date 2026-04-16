# Sonivox EAS — embedded audio synthesizer.
# Upstream: https://android.googlesource.com/platform/external/sonivox
# License: Apache-2.0 (see NOTICE).
# Mirror of AOSP's Android.bp build config, with ARM assembly omitted
# (the library has portable C fallbacks).

cmake_minimum_required(VERSION 3.20)

set(EAS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/arm-wt-22k)

set(SONIVOX_SOURCES
    ${EAS_ROOT}/lib_src/eas_data.c
    ${EAS_ROOT}/lib_src/eas_dlssynth.c
    ${EAS_ROOT}/lib_src/eas_flog.c
    ${EAS_ROOT}/lib_src/eas_imelody.c
    ${EAS_ROOT}/lib_src/eas_imelodydata.c
    ${EAS_ROOT}/lib_src/eas_math.c
    ${EAS_ROOT}/lib_src/eas_mdls.c
    ${EAS_ROOT}/lib_src/eas_midi.c
    ${EAS_ROOT}/lib_src/eas_mididata.c
    ${EAS_ROOT}/lib_src/eas_mixbuf.c
    ${EAS_ROOT}/lib_src/eas_mixer.c
    ${EAS_ROOT}/lib_src/eas_ota.c
    ${EAS_ROOT}/lib_src/eas_otadata.c
    ${EAS_ROOT}/lib_src/eas_pan.c
    ${EAS_ROOT}/lib_src/eas_pcm.c
    ${EAS_ROOT}/lib_src/eas_pcmdata.c
    ${EAS_ROOT}/lib_src/eas_public.c
    ${EAS_ROOT}/lib_src/eas_reverb.c
    ${EAS_ROOT}/lib_src/eas_reverbdata.c
    ${EAS_ROOT}/lib_src/eas_rtttl.c
    ${EAS_ROOT}/lib_src/eas_rtttldata.c
    ${EAS_ROOT}/lib_src/eas_smf.c
    ${EAS_ROOT}/lib_src/eas_smfdata.c
    ${EAS_ROOT}/lib_src/eas_voicemgt.c
    ${EAS_ROOT}/lib_src/eas_wtengine.c
    ${EAS_ROOT}/lib_src/eas_wtsynth.c
    ${EAS_ROOT}/lib_src/eas_xmf.c
    ${EAS_ROOT}/lib_src/eas_xmfdata.c
    ${EAS_ROOT}/lib_src/wt_22khz.c
    ${EAS_ROOT}/host_src/eas_config.c
    ${EAS_ROOT}/host_src/eas_hostmm.c
    ${EAS_ROOT}/host_src/eas_report.c
)

add_library(sonivox_eas STATIC ${SONIVOX_SOURCES})

target_include_directories(sonivox_eas PUBLIC
    ${EAS_ROOT}/include
)
target_include_directories(sonivox_eas PRIVATE
    ${EAS_ROOT}/host_src
    ${EAS_ROOT}/lib_src
    ${CMAKE_CURRENT_SOURCE_DIR}/compat    # shim for Android <log/log.h>
)

target_compile_definitions(sonivox_eas PRIVATE
    UNIFIED_DEBUG_MESSAGES
    EAS_WT_SYNTH
    _IMELODY_PARSER
    _RTTTL_PARSER
    _OTA_PARSER
    _XMF_PARSER
    NUM_OUTPUT_CHANNELS=2
    _SAMPLE_RATE_22050
    MAX_SYNTH_VOICES=64
    _16_BIT_SAMPLES
    _FILTER_ENABLED
    DLS_SYNTHESIZER
    _REVERB_ENABLED
)

# EAS is legacy C code — silence warnings that don't apply here so our -Werror-ish
# parent build doesn't choke on vendor code we don't want to patch.
target_compile_options(sonivox_eas PRIVATE
    -w
    -include stdbool.h       # AOSP Soong adds this implicitly; plain gcc doesn't
)

# Address sanitizer in the parent project should not apply to vendored C code
# that has been stable for 15+ years; but the flags are added globally, so we
# leave them on. If it proves noisy, we can suppress with an ASAN blacklist.
