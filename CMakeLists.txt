cmake_minimum_required(VERSION 3.24)
project(InspectMap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Sql Widgets)
find_program(QT_WINDEPLOYQT_EXECUTABLE NAMES windeployqt
    HINTS "${Qt6_DIR}/../../../bin" REQUIRED)
find_program(QT_QTPATHS_EXECUTABLE NAMES qtpaths6 qtpaths
    HINTS "${Qt6_DIR}/../../../bin" REQUIRED)

function(deploy_qt_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${QT_WINDEPLOYQT_EXECUTABLE}" --qtpaths "${QT_QTPATHS_EXECUTABLE}"
            --no-translations --$<IF:$<CONFIG:Debug>,debug,release> "$<TARGET_FILE:${target}>"
        VERBATIM)
endfunction()
set(OpenCV_ROOT "" CACHE PATH "OpenCV C++ installation root")
find_path(OpenCV_INCLUDE_DIR NAMES opencv2/opencv.hpp HINTS ${OpenCV_ROOT} ENV OpenCV_ROOT PATH_SUFFIXES include REQUIRED)
find_library(OpenCV_RELEASE_LIBRARY NAMES opencv_world460 opencv_world4 HINTS ${OpenCV_ROOT} ENV OpenCV_ROOT PATH_SUFFIXES lib REQUIRED)
find_library(OpenCV_DEBUG_LIBRARY NAMES opencv_world460d opencv_world4d HINTS ${OpenCV_ROOT} ENV OpenCV_ROOT PATH_SUFFIXES debug/lib lib)
find_file(OpenCV_RELEASE_DLL NAMES opencv_world460.dll opencv_world4.dll HINTS ${OpenCV_ROOT} ENV OpenCV_ROOT PATH_SUFFIXES bin REQUIRED)
find_file(OpenCV_DEBUG_DLL NAMES opencv_world460d.dll opencv_world4d.dll HINTS ${OpenCV_ROOT} ENV OpenCV_ROOT PATH_SUFFIXES debug/bin bin)
if(NOT OpenCV_DEBUG_LIBRARY)
    set(OpenCV_DEBUG_LIBRARY ${OpenCV_RELEASE_LIBRARY})
    set(OpenCV_DEBUG_DLL ${OpenCV_RELEASE_DLL})
endif()
add_library(OpenCVWorld SHARED IMPORTED)
set_target_properties(OpenCVWorld PROPERTIES
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
    MAP_IMPORTED_CONFIG_MINSIZEREL Release
    IMPORTED_IMPLIB_RELEASE "${OpenCV_RELEASE_LIBRARY}"
    IMPORTED_IMPLIB_DEBUG "${OpenCV_DEBUG_LIBRARY}"
    IMPORTED_LOCATION_RELEASE "${OpenCV_RELEASE_DLL}"
    IMPORTED_LOCATION_DEBUG "${OpenCV_DEBUG_DLL}")

add_library(InspectCore STATIC core/project_store.cpp core/project_store.h
    core/conversion_core.cpp core/conversion_core.h core/input_adapters.cpp core/input_adapters.h
    core/image_correction.cpp core/image_correction.h core/damage_store.cpp core/damage_store.h)
target_link_libraries(InspectCore PUBLIC Qt6::Core Qt6::Gui Qt6::Sql OpenCVWorld)
target_include_directories(InspectCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core ${OpenCV_INCLUDE_DIR})

add_executable(InspectionCli cli/main.cpp)
target_link_libraries(InspectionCli PRIVATE InspectCore)
add_custom_command(TARGET InspectionCli POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:OpenCVWorld> $<TARGET_FILE_DIR:InspectionCli>)
deploy_qt_runtime(InspectionCli)

add_executable(ProjectManager WIN32 apps/project_manager.cpp)
target_link_libraries(ProjectManager PRIVATE InspectCore Qt6::Widgets)
deploy_qt_runtime(ProjectManager)

add_executable(AIDamageManager WIN32 apps/ai_damage_manager.cpp)
target_link_libraries(AIDamageManager PRIVATE InspectCore Qt6::Widgets)
deploy_qt_runtime(AIDamageManager)

add_executable(DataConverter WIN32 apps/data_converter.cpp apps/lidar_preview.cpp apps/lidar_preview.h)
target_link_libraries(DataConverter PRIVATE InspectCore Qt6::Widgets)
find_package(VTK REQUIRED COMPONENTS CommonCore CommonDataModel CommonTransforms
    FiltersCore FiltersGeneral FiltersSources IOGeometry
    RenderingCore RenderingOpenGL2 InteractionStyle GUISupportQt)
if(NOT TARGET VTK::GUISupportQt OR VTK_QT_VERSION VERSION_LESS 6)
    message(FATAL_ERROR "DataConverter requires VTK built with Qt 6 support.")
endif()
find_package(Qt6 6.10 REQUIRED COMPONENTS OpenGLWidgets)
target_compile_definitions(DataConverter PRIVATE INSPECTMAP_WITH_VTK)
target_link_libraries(DataConverter PRIVATE Qt6::OpenGLWidgets
    VTK::CommonCore VTK::CommonDataModel VTK::FiltersSources
    VTK::RenderingCore VTK::RenderingOpenGL2 VTK::InteractionStyle VTK::GUISupportQt)
vtk_module_autoinit(TARGETS DataConverter MODULES ${VTK_LIBRARIES})
get_filename_component(VTK_RUNTIME_ROOT "${VTK_DIR}/../../.." ABSOLUTE)
file(GLOB VTK_RUNTIME_DLLS "${VTK_RUNTIME_ROOT}/bin/*.dll")
if(VTK_RUNTIME_DLLS)
    add_custom_command(TARGET DataConverter POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${VTK_RUNTIME_DLLS} $<TARGET_FILE_DIR:DataConverter> VERBATIM)
endif()
add_custom_command(TARGET DataConverter POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:OpenCVWorld> $<TARGET_FILE_DIR:DataConverter>)
deploy_qt_runtime(DataConverter)

add_library(RegistrationEngine STATIC apps/registration_engine.cpp apps/registration_engine.h)
target_link_libraries(RegistrationEngine PUBLIC Qt6::Core Qt6::Gui
    VTK::CommonCore VTK::CommonDataModel VTK::CommonTransforms
    VTK::FiltersCore VTK::FiltersGeneral VTK::IOGeometry)
target_include_directories(RegistrationEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/apps)

add_executable(RegistrationTool WIN32 apps/registration_tool.cpp apps/lidar_preview.cpp apps/lidar_preview.h)
target_compile_definitions(RegistrationTool PRIVATE INSPECTMAP_WITH_VTK)
target_link_libraries(RegistrationTool PRIVATE RegistrationEngine InspectCore Qt6::Widgets Qt6::Sql
    Qt6::OpenGLWidgets VTK::RenderingCore VTK::RenderingOpenGL2
    VTK::InteractionStyle VTK::GUISupportQt VTK::FiltersSources)
vtk_module_autoinit(TARGETS RegistrationTool MODULES ${VTK_LIBRARIES})
add_custom_command(TARGET RegistrationTool POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${VTK_RUNTIME_DLLS} $<TARGET_FILE_DIR:RegistrationTool> VERBATIM)
add_custom_command(TARGET RegistrationTool POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:OpenCVWorld> $<TARGET_FILE_DIR:RegistrationTool>)
deploy_qt_runtime(RegistrationTool)

add_executable(RegistrationEngineTest cli/registration_engine_test.cpp)
set_target_properties(RegistrationEngineTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
target_link_libraries(RegistrationEngineTest PRIVATE RegistrationEngine)
add_custom_command(TARGET RegistrationEngineTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${VTK_RUNTIME_DLLS} $<TARGET_FILE_DIR:RegistrationEngineTest> VERBATIM)
deploy_qt_runtime(RegistrationEngineTest)

add_executable(RegistrationPreviewUiTest cli/registration_preview_ui_test.cpp
    apps/lidar_preview.cpp apps/lidar_preview.h)
target_compile_definitions(RegistrationPreviewUiTest PRIVATE INSPECTMAP_WITH_VTK)
target_link_libraries(RegistrationPreviewUiTest PRIVATE RegistrationEngine InspectCore Qt6::Widgets Qt6::Sql
    Qt6::OpenGLWidgets VTK::RenderingCore VTK::RenderingOpenGL2
    VTK::InteractionStyle VTK::GUISupportQt VTK::FiltersSources)
vtk_module_autoinit(TARGETS RegistrationPreviewUiTest MODULES ${VTK_LIBRARIES})
add_custom_command(TARGET RegistrationPreviewUiTest POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${VTK_RUNTIME_DLLS} $<TARGET_FILE_DIR:RegistrationPreviewUiTest> VERBATIM)
add_custom_command(TARGET RegistrationPreviewUiTest POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:OpenCVWorld> $<TARGET_FILE_DIR:RegistrationPreviewUiTest>)
deploy_qt_runtime(RegistrationPreviewUiTest)

add_library(InspectionMapEngine STATIC apps/inspection_map_engine.cpp apps/inspection_map_engine.h
    apps/inspection_map_workflow.cpp apps/inspection_map_workflow.h)
target_link_libraries(InspectionMapEngine PUBLIC Qt6::Core Qt6::Gui Qt6::Sql)
target_include_directories(InspectionMapEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/apps)

add_executable(InspectionMapEditor WIN32 apps/inspection_map_editor.cpp)
target_link_libraries(InspectionMapEditor PRIVATE InspectionMapEngine InspectCore Qt6::Widgets)
add_custom_command(TARGET InspectionMapEditor POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:OpenCVWorld> $<TARGET_FILE_DIR:InspectionMapEditor>)
deploy_qt_runtime(InspectionMapEditor)

add_executable(InspectionMapEngineTest cli/inspection_map_engine_test.cpp)
target_link_libraries(InspectionMapEngineTest PRIVATE InspectionMapEngine)
deploy_qt_runtime(InspectionMapEngineTest)


enable_testing()
add_test(NAME project_store_smoke COMMAND InspectionCli self-test)
add_test(NAME converter_smoke COMMAND InspectionCli converter-self-test)
add_test(NAME registration_engine COMMAND RegistrationEngineTest)
add_test(NAME registration_preview_ui COMMAND RegistrationPreviewUiTest
    "${CMAKE_SOURCE_DIR}/Test/Project/project.sqlite")
add_test(NAME inspection_map_engine COMMAND InspectionMapEngineTest
    "${CMAKE_SOURCE_DIR}/Test/Data/model")

add_executable(InspectionMapWorkflowTest cli/inspection_map_workflow_test.cpp)
target_link_libraries(InspectionMapWorkflowTest PRIVATE InspectionMapEngine)
deploy_qt_runtime(InspectionMapWorkflowTest)
add_test(NAME inspection_map_workflow COMMAND InspectionMapWorkflowTest
    "${CMAKE_SOURCE_DIR}/Test/Project/project.sqlite" "${CMAKE_SOURCE_DIR}/Test/Data/model")

add_executable(InspectionMapUiTest cli/inspection_map_ui_test.cpp)
target_link_libraries(InspectionMapUiTest PRIVATE InspectionMapEngine InspectCore Qt6::Widgets)
add_custom_command(TARGET InspectionMapUiTest POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:OpenCVWorld> $<TARGET_FILE_DIR:InspectionMapUiTest>)
deploy_qt_runtime(InspectionMapUiTest)
add_test(NAME inspection_map_ui COMMAND InspectionMapUiTest
    "${CMAKE_SOURCE_DIR}/Test/Project/project.sqlite")

add_executable(DamageStoreTest cli/damage_store_test.cpp)
target_link_libraries(DamageStoreTest PRIVATE InspectCore)
add_custom_command(TARGET DamageStoreTest POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:OpenCVWorld> $<TARGET_FILE_DIR:DamageStoreTest>)
deploy_qt_runtime(DamageStoreTest)
add_test(NAME damage_store COMMAND DamageStoreTest "${CMAKE_SOURCE_DIR}/Test/Data/detect/json")

add_executable(AIDamageUiTest cli/ai_damage_ui_test.cpp)
target_link_libraries(AIDamageUiTest PRIVATE InspectCore Qt6::Widgets)
add_custom_command(TARGET AIDamageUiTest POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:OpenCVWorld> $<TARGET_FILE_DIR:AIDamageUiTest>)
deploy_qt_runtime(AIDamageUiTest)
add_test(NAME damage_ui_review_switch COMMAND AIDamageUiTest
    "${CMAKE_SOURCE_DIR}/Test/Project/project.sqlite" "${CMAKE_SOURCE_DIR}/Test/Data/detect/json")
