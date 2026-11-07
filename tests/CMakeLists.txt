set(SC_TEST_LINK_LIBRARIES sc-mime)

add_sc_test(messages LABELS unit)
add_sc_test(parts LABELS unit)
add_sc_test(drafts LABELS unit)
add_sc_test(outlook LABELS unit)
target_compile_definitions(test-outlook PRIVATE SC_MIME_TEST_RESOURCE="${CMAKE_CURRENT_SOURCE_DIR}/resource")
add_sc_test(delivery_report LABELS unit)
target_compile_definitions(test-delivery_report PRIVATE SC_MIME_TEST_RESOURCE="${CMAKE_CURRENT_SOURCE_DIR}/resource")
