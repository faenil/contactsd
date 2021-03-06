TEMPLATE = lib
QT -= gui
QT += dbus
QT += contacts-private

CONFIG += plugin

CONFIG += link_pkgconfig
PKGCONFIG += buteosyncfw5
PKGCONFIG += mlite5
PKGCONFIG += qtcontacts-sqlite-qt5-extensions
DEFINES *= USING_QTPIM QTCONTACTS_SQLITE_PERFORM_AGGREGATION

DEFINES -= QT_NO_CAST_TO_ASCII
DEFINES -= QT_NO_CAST_FROM_ASCII

INCLUDEPATH += $$TOP_SOURCEDIR/src
DEFINES += ENABLE_DEBUG

HEADERS  = \
    cdexportercontroller.h \
    cdexporterplugin.h

SOURCES  = \
    cdexportercontroller.cpp \
    cdexporterplugin.cpp

TARGET = exporterplugin
target.path = $$LIBDIR/contactsd-1.0/plugins

INSTALLS += target
