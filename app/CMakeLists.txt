juce_add_gui_app (spinus
    PRODUCT_NAME "Spinus"
    COMPANY_NAME "Spinus DAW"
    BUNDLE_ID "io.github.tuturu742.spinus"
    VERSION ${PROJECT_VERSION}
    ICON_BIG "${PROJECT_SOURCE_DIR}/packaging/share/spinus.svg"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

target_sources (spinus PRIVATE
    Main.cpp)

target_link_libraries (spinus PRIVATE spinus_core spinus_warnings)

# Put the executable at build/<preset>/spinus instead of deep inside the JUCE artefacts tree.
set_target_properties (spinus PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    OUTPUT_NAME spinus)

# Scans one plugin file per run, out of process. Installed next to spinus.
add_executable (spinus-plugin-scanner PluginScannerMain.cpp)
target_link_libraries (spinus-plugin-scanner PRIVATE spinus_core spinus_warnings)
set_target_properties (spinus-plugin-scanner PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Runs one plugin in its own process for Spinus. Installed next to spinus.
add_executable (spinus-plugin-host PluginHostMain.cpp)
target_link_libraries (spinus-plugin-host PRIVATE spinus_core spinus_warnings)
set_target_properties (spinus-plugin-host PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
