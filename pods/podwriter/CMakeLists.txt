# Pod Writer - distraction-free text editor, on the PicoPods libraries.
# On boards without Wi-Fi, pd_net reports "No Wi-Fi on this board" (F8/F9).
set(_libs pd_diag pd_text pd_epub pd_wifi pd_crosspoint)
picopods_add_pod(podwriter
    SOURCES src/main.cpp src/app.cpp src/app_share.cpp
    LIBS ${_libs}
    TITLE "Pod Writer"
    VERSION "3.0.0")
target_include_directories(podwriter PRIVATE src)
if(NOT PD_HOST)
    target_compile_definitions(podwriter PRIVATE CYW43_HOST_NAME=\"PodWriter\")
endif()
