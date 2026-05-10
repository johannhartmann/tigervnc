# This file is included from the top-level CMakeLists.txt.  We just store it
# here to avoid cluttering up that file.


#
# Windows installer (Inno Setup)
#

if(WIN32)

# Pick a per-arch suffix and matching ISPP define, so the generated .iss can
# emit the correct Architectures* lines and the output filename includes the
# target architecture.
if(CMAKE_SIZEOF_VOID_P MATCHES 8)
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _tigervnc_proc)
  if(_tigervnc_proc MATCHES "arm64|aarch64")
    set(INST_SUFFIX -arm64)
    set(INST_DEFS -DWIN64 -DWINARM64)
  else()
    set(INST_SUFFIX 64)
    set(INST_DEFS -DWIN64)
  endif()
endif()

# Forward ENABLE_NLS to the .iss preprocessor so it only tries to bundle the
# translation .mo files when they actually got built.
if(ENABLE_NLS)
  list(APPEND INST_DEFS -DWITH_NLS)
endif()

# When BUILD_STATIC is off the viewer / server pull in MinGW runtime DLLs
# (FLTK, GnuTLS chain, libjpeg, ...). Generate an .iss fragment listing them
# so the installer is self-contained on a vanilla Windows host. Static builds
# produce no resolved deps and the fragment ends up empty (harmless).
get_filename_component(_tigervnc_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)

set(_collect_script "${CMAKE_SOURCE_DIR}/release/collect_runtime_deps.cmake")

configure_file(tigervnc.iss.in tigervnc.iss)

add_custom_command(OUTPUT tigervnc-runtime-deps.iss
  COMMAND ${CMAKE_COMMAND}
    -DBINARIES=$<TARGET_FILE:vncviewer>
    -DDIRECTORIES=${_tigervnc_compiler_dir}
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/tigervnc-runtime-deps.iss
    -P ${_collect_script}
  DEPENDS vncviewer ${_collect_script}
  VERBATIM)

add_custom_command(OUTPUT ${CMAKE_PROJECT_NAME}${INST_SUFFIX}-${VERSION}.exe
  COMMAND iscc -o. ${INST_DEFS} -F${CMAKE_PROJECT_NAME}${INST_SUFFIX}-${VERSION} tigervnc.iss
  DEPENDS vncviewer tigervnc.iss tigervnc-runtime-deps.iss)
add_custom_target(installer DEPENDS ${CMAKE_PROJECT_NAME}${INST_SUFFIX}-${VERSION}.exe)

if(BUILD_WINVNC)
  configure_file(winvnc.iss.in winvnc.iss)

  add_custom_command(OUTPUT winvnc-runtime-deps.iss
    COMMAND ${CMAKE_COMMAND}
      -DBINARIES=$<TARGET_FILE:winvnc4>$<SEMICOLON>$<TARGET_FILE:vncconfig>$<SEMICOLON>$<TARGET_FILE:wm_hooks>
      -DDIRECTORIES=${_tigervnc_compiler_dir}
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/winvnc-runtime-deps.iss
      -P ${_collect_script}
    DEPENDS winvnc4 vncconfig wm_hooks ${_collect_script}
    VERBATIM)

  add_custom_command(OUTPUT ${CMAKE_PROJECT_NAME}${INST_SUFFIX}-winvnc-${VERSION}.exe
    COMMAND iscc -o. ${INST_DEFS} -F${CMAKE_PROJECT_NAME}${INST_SUFFIX}-winvnc-${VERSION} winvnc.iss
    DEPENDS winvnc4 wm_hooks vncconfig winvnc.iss winvnc-runtime-deps.iss)
  add_custom_target(winvnc_installer DEPENDS ${CMAKE_PROJECT_NAME}${INST_SUFFIX}-winvnc-${VERSION}.exe)
endif()

endif() # WIN32


#
# Mac DMG
#

if(APPLE)

configure_file(makemacapp.in makemacapp)
configure_file(Info.plist.in Info.plist)

add_custom_command(OUTPUT TigerVNC-${VERSION}.dmg
  COMMAND sh makemacapp
  DEPENDS vncviewer makemacapp Info.plist)
add_custom_target(dmg DEPENDS TigerVNC-${VERSION}.dmg)

endif() # APPLE


#
# Binary tarball
#

if(UNIX)

configure_file(maketarball.in maketarball)

if(BUILD_JAVA)
  set(TARBALL_JAVA_DEPENDENCY java)
endif()

set(PACKAGE_FILE ${CMAKE_PROJECT_NAME}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-${VERSION}.tar.gz)
add_custom_command(OUTPUT ${PACKAGE_FILE}
  COMMAND bash maketarball
  DEPENDS maketarball vncviewer vncpasswd vncconfig ${TARBALL_JAVA_DEPENDENCY})

add_custom_target(tarball DEPENDS ${PACKAGE_FILE})

endif() #UNIX

#
# Common
#

install(FILES ${CMAKE_SOURCE_DIR}/LICENCE.TXT DESTINATION ${CMAKE_INSTALL_FULL_DOCDIR})
install(FILES ${CMAKE_SOURCE_DIR}/README.rst DESTINATION ${CMAKE_INSTALL_FULL_DOCDIR})
