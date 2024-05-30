target_sources(kwin PRIVATE
    hwcomposer_egl_backend.cpp
    hwcomposer_backend.cpp
    hwcomposer_logging.cpp
)

include_directories(${CMAKE_SOURCE_DIR}/platformsupport/scenes/opengl)


target_link_libraries(kwin
    libhybris::hwcomposer
    libhybris::hybriseglplatform
    libhybris::libhardware
    -lhwc2 -lsync
    dl
)
