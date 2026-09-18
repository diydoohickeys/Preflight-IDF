cmake_minimum_required(VERSION 3.16)

# The repository root is the component. `main` declares no REQUIRES, so it sees every
# component, whatever the checkout folder is called.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(preflight_basic)
