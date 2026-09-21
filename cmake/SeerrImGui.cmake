# Dear ImGui + GLFW (static imgui, system or vendored GLFW).

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
set(GLFW_DIR ${CMAKE_SOURCE_DIR}/vendor/glfw)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC GLFW_INCLUDE_NONE)

if(WIN32)
    target_include_directories(imgui PUBLIC ${GLFW_DIR}/include)
    target_link_libraries(imgui PUBLIC ${GLFW_DIR}/lib-mingw-w64/libglfw3.a opengl32 gdi32)
else()
    # Do not link OpenGL::GL — GLVND stubs segfault if called without a loader.
    find_package(glfw3 REQUIRED)
    set(SEERR_GLFW_TARGET "")
    if(TARGET glfw)
        set(SEERR_GLFW_TARGET glfw)
    elseif(TARGET glfw3)
        set(SEERR_GLFW_TARGET glfw3)
    elseif(TARGET GLFW::glfw)
        set(SEERR_GLFW_TARGET GLFW::glfw)
    else()
        message(FATAL_ERROR "glfw3 found but no usable CMake target (glfw / glfw3 / GLFW::glfw)")
    endif()
    target_link_libraries(imgui PUBLIC ${SEERR_GLFW_TARGET} ${CMAKE_DL_LIBS})
    message(STATUS "Seerr GLFW target: ${SEERR_GLFW_TARGET}")
endif()
