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

# Pod Writer accent keys (F10): ' and ` are dead keys for accented vowels.
file(MAKE_DIRECTORY ${_tmp}/podwriter_acc_sd)
set(_akeys "wait:100,text:l'a,f10,wait:100,text:perch'e citt`a `E 'U l' una `` 'x,bksp,bksp,text:'s,shot,ctrl+s,ctrl+u,text:/acc.txt,enter")
add_test(NAME podwriter_accents COMMAND ${CMAKE_COMMAND} -E env
    PD_SDCARD=${_tmp}/podwriter_acc_sd PD_KEYS=${_akeys}
    sh -c "rm -rf ${_tmp}/podwriter_acc_sd/* ${_tmp}/podwriter_acc_sd/.fwedit && $<TARGET_FILE:podwriter> && cmp ${_tmp}/podwriter_acc_sd/acc.txt ${CMAKE_CURRENT_SOURCE_DIR}/podwriter_accents.txt && grep -q accents=1 ${_tmp}/podwriter_acc_sd/.fwedit/settings.txt")
set_tests_properties(podwriter_accents PROPERTIES TIMEOUT 60)

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

# Pod Tunes: place notes from the keyboard (Z on LEAD, X on ARP after Tab,
# Z again on pattern P2 after ]), play and stop, export the song as JSON and
# render a WAV; the card must hold the exported files and the autosaved song.
file(MAKE_DIRECTORY ${_tmp}/podtunes_sd)
set(_tkeys "wait:400,z,tab,x,],z,space,wait:700,space,wait:500")
set(_tkeys "${_tkeys},ctrl+e,wait:100,ctrl+u,text:/song.json,enter,wait:400")
set(_tkeys "${_tkeys},ctrl+w,wait:100,ctrl+u,text:/out.wav,enter,wait:3000")
add_test(NAME podtunes_script COMMAND ${CMAKE_COMMAND} -E env
    PD_SDCARD=${_tmp}/podtunes_sd PD_FRAMES=${_tmp}/podtunes_frames PD_KEYS=${_tkeys}
    sh -c "rm -rf ${_tmp}/podtunes_sd/* ${_tmp}/podtunes_sd/.podtunes && $<TARGET_FILE:podtunes> && test -s ${_tmp}/podtunes_sd/.podtunes/song.json && grep -q 'PIXEL PLAINS ZONE' ${_tmp}/podtunes_sd/.podtunes/song.json && grep -q '\"n\":\"C4\"' ${_tmp}/podtunes_sd/.podtunes/song.json && grep -q '\"n\":\"D4\"' ${_tmp}/podtunes_sd/.podtunes/song.json && grep -q '\"arp\":{\"0\":{\"n\":\"C4\"' ${_tmp}/podtunes_sd/.podtunes/song.json && test -s ${_tmp}/podtunes_sd/song.json && grep -q '\"app\":\"pod-tunes\"' ${_tmp}/podtunes_sd/song.json && head -c 4 ${_tmp}/podtunes_sd/out.wav | grep -q RIFF && test $(stat -c %s ${_tmp}/podtunes_sd/out.wav) -gt 100000")
set_tests_properties(podtunes_script PROPERTIES TIMEOUT 60)

# Pod Tunes: import a song JSON in the web creator's format through the file
# browser; the autosaved song must become the imported one.
set(_isd ${_tmp}/podtunes_import_sd)
add_test(NAME podtunes_import COMMAND ${CMAKE_COMMAND} -E env
    PD_SDCARD=${_isd} "PD_KEYS=wait:400,ctrl+i,wait:300,enter,wait:700"
    sh -c "rm -rf ${_isd} && mkdir -p ${_isd} && cp ${CMAKE_CURRENT_SOURCE_DIR}/podtunes_import.json ${_isd}/tune.json && $<TARGET_FILE:podtunes> && grep -q 'IMPORTED THEME' ${_isd}/.podtunes/song.json && grep -q '\"n\":\"A4\"' ${_isd}/.podtunes/song.json && grep -q '\"0\":\"kick\"' ${_isd}/.podtunes/song.json && grep -q '\[0,0\]' ${_isd}/.podtunes/song.json")
set_tests_properties(podtunes_import PROPERTIES TIMEOUT 60)

# Pairing started by the phone while the device list is open: the request must
# be shown and answered from inside the menu (ui idle hook + key::Repaint).
if(PD_HAS_BT)
    add_test(NAME podfiles_bt_phone_pairing COMMAND ${CMAKE_COMMAND} -E env
        PD_SDCARD=${_tmp}/btp_sd PD_BT_SIM_PHONE_PAIR=1
        "PD_KEYS=wait:100,b,wait:50,enter,wait:3000,enter,wait:200,esc,wait:100"
        sh -c "rm -rf ${_tmp}/btp_sd && mkdir -p ${_tmp}/btp_sd && echo hi > ${_tmp}/btp_sd/doc.txt && $<TARGET_FILE:podfiles>")
    set_tests_properties(podfiles_bt_phone_pairing PROPERTIES PASS_REGULAR_EXPRESSION "pairing answered: yes" TIMEOUT 60)
endif()
