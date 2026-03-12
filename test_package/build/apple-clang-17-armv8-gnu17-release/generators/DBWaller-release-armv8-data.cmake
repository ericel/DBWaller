########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

set(dbwaller_COMPONENT_NAMES "")
if(DEFINED dbwaller_FIND_DEPENDENCY_NAMES)
  list(APPEND dbwaller_FIND_DEPENDENCY_NAMES spdlog OpenSSL)
  list(REMOVE_DUPLICATES dbwaller_FIND_DEPENDENCY_NAMES)
else()
  set(dbwaller_FIND_DEPENDENCY_NAMES spdlog OpenSSL)
endif()
set(spdlog_FIND_MODE "NO_MODULE")
set(OpenSSL_FIND_MODE "NO_MODULE")

########### VARIABLES #######################################################################
#############################################################################################
set(dbwaller_PACKAGE_FOLDER_RELEASE "/Users/ojobasi/.conan2/p/b/dbwal4e8df18359c60/p")
set(dbwaller_BUILD_MODULES_PATHS_RELEASE )


set(dbwaller_INCLUDE_DIRS_RELEASE "${dbwaller_PACKAGE_FOLDER_RELEASE}/include")
set(dbwaller_RES_DIRS_RELEASE )
set(dbwaller_DEFINITIONS_RELEASE )
set(dbwaller_SHARED_LINK_FLAGS_RELEASE )
set(dbwaller_EXE_LINK_FLAGS_RELEASE )
set(dbwaller_OBJECTS_RELEASE )
set(dbwaller_COMPILE_DEFINITIONS_RELEASE )
set(dbwaller_COMPILE_OPTIONS_C_RELEASE )
set(dbwaller_COMPILE_OPTIONS_CXX_RELEASE )
set(dbwaller_LIB_DIRS_RELEASE "${dbwaller_PACKAGE_FOLDER_RELEASE}/lib")
set(dbwaller_BIN_DIRS_RELEASE )
set(dbwaller_LIBRARY_TYPE_RELEASE STATIC)
set(dbwaller_IS_HOST_WINDOWS_RELEASE 0)
set(dbwaller_LIBS_RELEASE dbwaller)
set(dbwaller_SYSTEM_LIBS_RELEASE )
set(dbwaller_FRAMEWORK_DIRS_RELEASE )
set(dbwaller_FRAMEWORKS_RELEASE )
set(dbwaller_BUILD_DIRS_RELEASE )
set(dbwaller_NO_SONAME_MODE_RELEASE FALSE)


# COMPOUND VARIABLES
set(dbwaller_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${dbwaller_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${dbwaller_COMPILE_OPTIONS_C_RELEASE}>")
set(dbwaller_LINKER_FLAGS_RELEASE
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${dbwaller_SHARED_LINK_FLAGS_RELEASE}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${dbwaller_SHARED_LINK_FLAGS_RELEASE}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${dbwaller_EXE_LINK_FLAGS_RELEASE}>")


set(dbwaller_COMPONENTS_RELEASE )