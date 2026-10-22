set(SC_TEST_LINK_LIBRARIES sc-tcp)

add_sc_test(server LABELS unit TIMEOUT 60)
add_sc_test(client LABELS unit TIMEOUT 60)
