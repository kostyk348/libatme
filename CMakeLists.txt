cmake_minimum_required(VERSION 3.16)
project(libatme C)

# libatme — Type 1 / PFB растеризатор. CMake-цели: статическая библиотека,
# тест корректности и бенчмарк. Сравнение с FreeType (atm_ft_bench) остаётся
# в Makefile: для него нужен pkg-config freetype2, которого может не быть.

option(LIBATME_BUILD_TESTS "Собрать тест рендера" ON)
option(LIBATME_BUILD_BENCH "Собрать бенчмарк" ON)

set(LIBATME_SOURCES
    atm_outline.c atm_raster.c atm_cache.c atm_gamma.c
    atm_engine.c atm_font.c atm_bench.c)

add_library(atme STATIC ${LIBATME_SOURCES})
target_include_directories(atme PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(atme PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(atme PRIVATE -Wall -Wextra -O2)
endif()
target_link_libraries(atme PUBLIC m)

if(LIBATME_BUILD_TESTS)
    enable_testing()

    # синтетический тест растеризатора: работает без файла шрифта
    add_executable(test_raster tests/test_raster.c)
    target_link_libraries(test_raster PRIVATE atme)
    if(NOT MSVC)
        target_compile_options(test_raster PRIVATE -Wall -Wextra -O2)
    endif()
    add_test(NAME libatme_raster COMMAND test_raster)

    # тест на реальном .pfb: пропускается, если шрифт не передан (LIBATME_PFB)
    add_executable(test_render test_render.c)
    target_link_libraries(test_render PRIVATE atme)
    if(NOT MSVC)
        target_compile_options(test_render PRIVATE -Wall -Wextra -O2)
    endif()
    add_test(NAME libatme_render_font COMMAND test_render)
endif()

if(LIBATME_BUILD_BENCH)
    add_executable(atm_bench_bin atm_bench.c)
    target_link_libraries(atm_bench_bin PRIVATE atme)
endif()
