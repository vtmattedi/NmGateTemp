FILE(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/src/*.c
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)

# The dashboard is linked into the firmware and served by src/webServer.
idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "webServer/WebSite/index.html"
        "webServer/WebSite/style.css"
        "webServer/WebSite/app.js"
        "webServer/WebSite/nightmare_logo.svg"
    PRIV_REQUIRES
        mdns
)