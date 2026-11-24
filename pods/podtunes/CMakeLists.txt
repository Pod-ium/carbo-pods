picopods_add_pod(podtunes
    SOURCES src/main.cpp src/tracker.cpp
    LIBS pd_diag
    TITLE "Pod Tunes"
    VERSION "1.0.0")
target_include_directories(podtunes PRIVATE src)
