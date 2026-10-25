# Host tests of the productivity pods (cmake -DPD_TERMINAL=host): scripted sessions
# that drive each pod through its screens. Run with ctest.
set(_tmp ${CMAKE_CURRENT_BINARY_DIR}/tmp)
file(MAKE_DIRECTORY ${_tmp})

# Pod Writer: type a document, save it, help, menu, theme, ePub export (tagged
# with the pod's name), Wi-Fi menu.
file(MAKE_DIRECTORY ${_tmp}/podwriter_sd)
set(_wkeys "wait:200,text:# Hello PicoPods,enter,enter,text:Some *text* here.,ctrl+s,ctrl+u,text:/hello.md,enter")
set(_wkeys "${_wkeys},f1,esc,f6,f5,esc,shot,esc,f7,enter,wait:200,f9,esc,shot")
add_test(NAME podwriter_script COMMAND ${CMAKE_COMMAND} -E env
    PD_SDCARD=${_tmp}/podwriter_sd PD_FRAMES=${_tmp}/podwriter_frames PD_KEYS=${_wkeys}
    sh -c "rm -rf ${_tmp}/podwriter_sd/* ${_tmp}/podwriter_sd/.fwedit && $<TARGET_FILE:podwriter> && test -s ${_tmp}/podwriter_sd/hello.md && test -s ${_tmp}/podwriter_sd/hello.epub && unzip -p ${_tmp}/podwriter_sd/hello.epub OEBPS/content.opf | grep -q 'content=\"Pod Writer\"'")
set_tests_properties(podwriter_script PROPERTIES TIMEOUT 60)

# Pod Files: make a folder, copy a file into it, cut/paste, delete, hidden
# toggle, menu, help; the card must end up in the expected state.
set(_fsd ${_tmp}/files_sd)
# (pauses after each operation: the app drops type-ahead once one finishes)
set(_fkeys "wait:100,n,ctrl+u,text:Box,enter,wait:100,down,down,c,up,up,enter,v,wait:100,v,wait:100,shot,down,enter,wait:100")
set(_fkeys "${_fkeys},bksp,down,x,up,enter,v,wait:100,shot,bksp,down,down,d,wait:100,shot,enter,wait:100")
set(_fkeys "${_fkeys},r,ctrl+u,text:Crate,enter,wait:100,esc,esc,wait:50,f1,esc,wait:50,i,wait:50,esc,wait:50")
set(_fkeys "${_fkeys},u,wait:50,esc,wait:50,h,h,s,s,s,shot")
add_test(NAME podfiles_script COMMAND ${CMAKE_COMMAND} -E env
    PD_SDCARD=${_fsd} PD_FRAMES=${_tmp}/files_frames PD_KEYS=${_fkeys}
    sh -c "rm -rf ${_fsd} ${_tmp}/files_frames && mkdir -p ${_fsd}/Docs ${_tmp}/files_frames && echo hello > ${_fsd}/Docs/a.txt && echo world > ${_fsd}/b.txt && echo x > ${_fsd}/c.txt && $<TARGET_FILE:podfiles> && sh ${CMAKE_CURRENT_SOURCE_DIR}/check_files.sh ${_fsd}")
set_tests_properties(podfiles_script PROPERTIES TIMEOUT 60)

# Pod Files on a card used by PicoCalc Files: the old /.picocalc_files folder
# (settings, Wi-Fi, Bluetooth pairings) is renamed to /.podfiles once.
set(_msd ${_tmp}/podfiles_migrate_sd)
add_test(NAME podfiles_migrate COMMAND ${CMAKE_COMMAND} -E env PD_SDCARD=${_msd} PD_KEYS=wait:100
    sh -c "rm -rf ${_msd} && mkdir -p ${_msd}/.picocalc_files && echo 'ssid=home' > ${_msd}/.picocalc_files/wifi.txt && $<TARGET_FILE:podfiles> && test ! -e ${_msd}/.picocalc_files && grep -q ssid=home ${_msd}/.podfiles/wifi.txt")
set_tests_properties(podfiles_migrate PROPERTIES TIMEOUT 60)

# Pod Files: receive a file over (simulated) Bluetooth - pairing prompt,
# then a real OBEX Object Push session through the real receiver. The file
# must arrive byte-identical, with its non-ASCII name.
if(PD_HAS_BT)
    set(_bsd ${_tmp}/bt_sd)
    add_test(NAME podfiles_bt_receive COMMAND ${CMAKE_COMMAND} -E env
        PD_SDCARD=${_bsd} PD_FRAMES=${_tmp}/bt_frames "PD_BT_SIM_FILE=${_tmp}/foto città.jpg"
        "PD_KEYS=wait:100,enter,wait:50,b,wait:50,enter,wait:1400,enter,wait:1500,esc,wait:200"
        sh -c "rm -rf ${_bsd} ${_tmp}/bt_frames && mkdir -p ${_bsd}/Inbox ${_tmp}/bt_frames && head -c 40000 /dev/urandom > '${_tmp}/foto città.jpg' && $<TARGET_FILE:podfiles> && cmp '${_tmp}/foto città.jpg' '${_bsd}/Inbox/foto città.jpg' && test ! -e '${_bsd}/Inbox/.bt-incoming.part'")
    set_tests_properties(podfiles_bt_receive PROPERTIES TIMEOUT 60)
endif()

# Pairing started by the phone while the device list is open: the request must
# be shown and answered from inside the menu (ui idle hook + key::Repaint).
if(PD_HAS_BT)
    add_test(NAME podfiles_bt_phone_pairing COMMAND ${CMAKE_COMMAND} -E env
        PD_SDCARD=${_tmp}/btp_sd PD_BT_SIM_PHONE_PAIR=1
        "PD_KEYS=wait:100,b,wait:50,enter,wait:3000,enter,wait:200,esc,wait:100"
        sh -c "rm -rf ${_tmp}/btp_sd && mkdir -p ${_tmp}/btp_sd && echo hi > ${_tmp}/btp_sd/doc.txt && $<TARGET_FILE:podfiles>")
    set_tests_properties(podfiles_bt_phone_pairing PROPERTIES PASS_REGULAR_EXPRESSION "pairing answered: yes" TIMEOUT 60)
endif()
