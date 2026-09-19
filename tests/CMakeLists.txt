# Самотесты ядра. Требуют ferry-crypto, то есть OpenSSL: без него проверять
# AEAD и вывод ключей нечем, а это самая важная часть проверки.
if(TARGET ferry-crypto)
    add_executable(ferry-core-test CoreTests.cpp)
    target_link_libraries(ferry-core-test PRIVATE ferry-core ferry-crypto)

    enable_testing()
    add_test(NAME core COMMAND ferry-core-test)
else()
    message(STATUS "OpenSSL не найден — самотесты ядра пропущены")
endif()
