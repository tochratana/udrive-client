include(owncloud_add_test.cmake)

add_subdirectory(testutils)

owncloud_add_test(JHash)

owncloud_add_test(OwncloudPropagator)
owncloud_add_test(OwnSql)
owncloud_add_test(SyncJournalDB)
owncloud_add_test(SyncFileItem)
owncloud_add_test(ConcatUrl)
owncloud_add_test(XmlParse)
owncloud_add_test(OAuthHtmlPage ../src/gui/creds/oauthhtmlpage.cpp)
owncloud_add_test(ChecksumValidator)
owncloud_add_test(ConnectionValidator)

# TODO: we need keychain access for this test
if (NOT APPLE OR NOT DEFINED ENV{GITHUB_ACTION})
    owncloud_add_test(CredentialManager)
endif()

owncloud_add_test(ExcludedFiles)

owncloud_add_test(Utility)
owncloud_add_test(AppConfig ../src/libsync/owncloudtheme.cpp)

owncloud_add_test(SyncEngine)
owncloud_add_test(SyncMove)
add_dependencies(SyncMoveTest test_helper)
owncloud_add_test(SyncDelete)
owncloud_add_test(SyncConflict)
owncloud_add_test(SyncFileStatusTracker)
owncloud_add_test(Download)
owncloud_add_test(UploadReset)
owncloud_add_test(Blacklist)
owncloud_add_test(LocalDiscovery)
owncloud_add_test(RemoteDiscovery)
owncloud_add_test(Permissions)
owncloud_add_test(DatabaseError)
owncloud_add_test(LockedFiles)
owncloud_add_test(Adapters
    ../src/gui/networkadapters/determineauthtypeadapter.cpp
    ../src/gui/networkadapters/discoverwebfingerserviceadapter.cpp
    ../src/gui/networkadapters/fetchcapabilitiesadapter.cpp
#    ../src/gui/networkadapters/resolveurladapter.cpp
#    ../src/gui/tlserrordialog.cpp
    ../src/gui/networkadapters/webfingerlookupadapter.cpp
)

if (WITH_AUTO_UPDATER)
    owncloud_add_test(Updater)
endif()

owncloud_add_test(FolderWatcher)

if( UNIX AND NOT APPLE )
    owncloud_add_test(InotifyWatcher)
endif(UNIX AND NOT APPLE)

owncloud_add_test(FileSystem)

owncloud_add_test(FolderMan)

owncloud_add_test(OAuth ../src/gui/creds/oauth.cpp ../src/gui/networkadapters/userinfoadapter.cpp ../src/gui/creds/oauthhtmlpage.cpp)

configure_file(test_journal.db "${PROJECT_BINARY_DIR}/bin/test_journal.db" COPYONLY)

owncloud_add_test(JobQueue)
set(udrive_pdf_sources)
if(APPLE)
    list(APPEND udrive_pdf_sources ../src/gui/udrivepdfpreview.cpp ../src/gui/udrivepdfdocument_mac.mm)
endif()
owncloud_add_test(UDriveFileBrowser ../src/gui/udrivefilebrowser.cpp ../src/gui/udrivethumbnails.cpp ${udrive_pdf_sources})
if(APPLE)
    target_link_libraries(UDriveFileBrowserTest "-framework CoreGraphics")
    add_dependencies(UDriveFileBrowserTest udrivePdfRenderer)
endif()
owncloud_add_test(LocalDevelopmentUrl)
owncloud_add_test(HttpLogger)
owncloud_add_test(Translations)
if(APPLE)
    ecm_add_test(testmacdock.mm TEST_NAME MacDockTest LINK_LIBRARIES owncloudGui syncenginetestutils testutilsloader Qt::Test)
    apply_common_target_settings(MacDockTest)
    target_compile_definitions(MacDockTest PRIVATE SOURCEDIR="${PROJECT_SOURCE_DIR}" QT_FORCE_ASSERTS)
    target_link_libraries(MacDockTest "-framework AppKit")
endif()
owncloud_add_test(CredentialSecurity)
ecm_add_test(testlocaldevelopmenturl.cpp TEST_NAME LocalDevelopmentUrlReleaseTest LINK_LIBRARIES Qt::Test)
target_include_directories(LocalDevelopmentUrlReleaseTest PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(LocalDevelopmentUrlReleaseTest PRIVATE QT_NO_DEBUG)

add_subdirectory(modeltests)

if(WIN32)
    find_file(cldapi_FOUND CldApi.dll)
    if (cldapi_FOUND)
        include("${CMAKE_SOURCE_DIR}/test/owncloud_add_test.cmake")
        owncloud_add_test(WinVfs)
        target_link_libraries(WinVfsTest vfs_plugin_win_static)
        add_dependencies(WinVfsTest vfs_wincfapi)
    else()
        message(WARNING "Failed to detect CldApi.dll: Skipping WinVfsTest")
    endif()
endif()
