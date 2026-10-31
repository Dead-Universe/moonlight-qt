QT += core gui network
CONFIG += c++17
CONFIG -= app_bundle

TARGET = moonlight-clipboard-helper
TEMPLATE = app

include(../globaldefs.pri)

INCLUDEPATH += $$PWD/../app

SOURCES += \
    main.cpp \
    ../app/streaming/clipboardipc.cpp \
    ../app/streaming/clipboardsync.cpp

macx:SOURCES += macos.mm

HEADERS += \
    ../app/streaming/clipboardipc.h \
    ../app/streaming/clipboardlogging.h \
    ../app/streaming/clipboardsync.h

unix:!macx {
    isEmpty(PREFIX) {
        PREFIX = /usr/local
    }
    isEmpty(BINDIR) {
        BINDIR = bin
    }

    target.path = $$PREFIX/$$BINDIR/
    INSTALLS += target
}

win32 {
    SOURCES += fileclipboardsync.cpp native/file_clipboard_native.cpp native/file_clipboard_source.cpp
    HEADERS += fileclipboardsync.h native/file_clipboard_native.h native/file_clipboard_wire.h
    DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    LIBS += -lole32 -loleaut32 -lshell32 -luuid -luser32 -ladvapi32 -lwtsapi32
}