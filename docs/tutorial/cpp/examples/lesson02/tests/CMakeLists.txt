add_executable(geometry_test test.cpp)
target_link_libraries(geometry_test PRIVATE geometry)

# 把这个程序登记为一个测试，之后可以用 ctest 运行
add_test(NAME geometry_test COMMAND geometry_test)
