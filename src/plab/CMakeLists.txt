# PhotographyLAB: a Qt 6 front end on darktable's engine (lib_darktable).
# it links the engine headless, so nothing here draws with GTK

find_package(Qt6 6.4 COMPONENTS Core Gui Quick QuickControls2 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PLAB_JSON_GLIB REQUIRED json-glib-1.0)

set(CMAKE_AUTOMOC ON)

qt_add_executable(photographylab
  main.cpp
  labcontroller.cpp
  labcontroller.h
  engineworker.cpp
  engineworker.h
  plab_engine.c
  plab_engine.h)

# must be set before qt_add_qml_module() writes the qmldir
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
# served next to the module's qmldir, so Main.qml's implicit import of its own
# folder finds the Theme singleton
foreach(f Main LibraryView AdjustSlider SectionHeader Theme)
  set_source_files_properties(qml/${f}.qml PROPERTIES QT_RESOURCE_ALIAS ${f}.qml)
endforeach()

qt_add_qml_module(photographylab
  URI PhotographyLab
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/LibraryView.qml
    qml/AdjustSlider.qml
    qml/SectionHeader.qml
    qml/Theme.qml
  # a PNG for the window: the AppImage carries no SVG image plugin
  RESOURCES
    icons/photographylab-256.png)

set_target_properties(photographylab PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})

# qmlcachegen's generated C++ shadows its own parameters, and darktable builds
# with -Werror
target_compile_options(photographylab PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-shadow>)

target_include_directories(photographylab PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${DARKTABLE_BINDIR}
  ${PLAB_JSON_GLIB_INCLUDE_DIRS})

target_link_libraries(photographylab PRIVATE
  lib_darktable
  Qt6::Core Qt6::Gui Qt6::Quick Qt6::QuickControls2)

if(NOT WIN32)
  set_target_properties(photographylab PROPERTIES
    INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR})
endif()

install(TARGETS photographylab DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)

install(FILES icons/photographylab.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
        COMPONENT DTApplication)
install(FILES icons/photographylab-256.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps
        RENAME photographylab.png
        COMPONENT DTApplication)

# the bundled looks, copied into the user's LUT collection on first start
file(GLOB PLAB_LUTS ${CMAKE_CURRENT_SOURCE_DIR}/luts/*.cube)
install(FILES ${PLAB_LUTS}
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/photographylab/luts
        COMPONENT DTApplication)
# copied into the build tree too, so the app finds them when run from there
file(COPY ${PLAB_LUTS} DESTINATION ${CMAKE_BINARY_DIR}/share/photographylab/luts)
