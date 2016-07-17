# Проверка версии cmake, выход если версия меньше 2.8.
cmake_minimum_required(VERSION 2.8)

# Имя проекта.
project(final)

# Флаги компиляции, для компилятора c++.
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

# В переменную LIBS добавляем подключаемые библиотеки.
set (LIBS "-lev -lpthread")

# Создать список файлов в папке ./src/ и записать его в переменную SRCS.
aux_source_directory(src SRCS)
# Добавление файлов по одному set(SRCS main.cpp)

# Создать проект с именем PROJECT_NAME согласно файлов из списка SRCS.
add_executable(${PROJECT_NAME} ${SRCS})

# Подключаем к проекту библиотеки.
target_link_libraries(${PROJECT_NAME} ${LIBS})
