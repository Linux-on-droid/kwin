target_sources(kwin PRIVATE
    egl_hwcomposer_backend.cpp
    hwcomposer_backend.cpp
    logging.cpp
)

include_directories(${CMAKE_SOURCE_DIR}/platformsupport/scenes/opengl)
target_link_libraries(kwin

    SceneOpenGLBackend

    libhybris::hwcomposer
    libhybris::hybriseglplatform
    libhybris::libhardware
)

