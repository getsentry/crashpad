add_executable(crashpad_client_link_test
    client_link_test.cc
)
set_property(TARGET crashpad_client_link_test PROPERTY WIN32_EXECUTABLE OFF)

target_link_libraries(crashpad_client_link_test PRIVATE
    crashpad_client
)

if(IOS)
    set_target_properties(crashpad_client_link_test PROPERTIES
        MACOSX_BUNDLE TRUE
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER org.crashpad.client-link-test
    )
endif()
