# Самотесты ядра. Требуют ferry-crypto, то есть OpenSSL: без него проверять
# AEAD и вывод ключей нечем, а это самая важная часть проверки.
if(TARGET ferry-crypto)
    add_executable(ferry-core-test CoreTests.cpp)
    target_link_libraries(ferry-core-test PRIVATE ferry-core ferry-crypto)

    # Выравнивание пароля обязано совпадать с браузером до байта — проверяем
    # его здесь же, если собирается клиент (Argon2 вендорен в Cli/).
    if(FERRY_BUILD_CLI)
        target_sources(ferry-core-test PRIVATE ${PROJECT_SOURCE_DIR}/Cli/Password.cpp)
        target_link_libraries(ferry-core-test PRIVATE ferry-argon2)
        target_compile_definitions(ferry-core-test PRIVATE FERRY_TEST_PASSWORD)
    endif()

    enable_testing()
    add_test(NAME core COMMAND ferry-core-test)
else()
    message(STATUS "OpenSSL не найден — самотесты ядра пропущены")
endif()
