# UDrive's visual identity. Keep configuration/keychain compatibility with the
# development client in UDriveTheme; the bundle and executable have their own names.
include("${PROJECT_SOURCE_DIR}/OWNCLOUD.cmake")
set(APPLICATION_NAME "UDrive")
set(APPLICATION_SHORTNAME "UDrive")
set(APPLICATION_EXECUTABLE "udrive")
set(APPLICATION_DOMAIN "drive.unity-workspace.com")
set(APPLICATION_VENDOR "Unity Workspace")
set(APPLICATION_REV_DOMAIN "com.unity-workspace.udrive")
set(APPLICATION_ICON_NAME "udrive")
set(THEME_CLASS "UDriveTheme")
set(THEME_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/udrivetheme.h")
# A fork must supply its own signed update feed before enabling updates.
set(WITHOUT_AUTO_UPDATER ON)
