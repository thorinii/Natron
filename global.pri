#This Source Code Form is subject to the terms of the Mozilla Public
#License, v. 2.0. If a copy of the MPL was not distributed with this
#file, You can obtain one at http://mozilla.org/MPL/2.0/.

CONFIG += warn_on no_keywords
DEFINES += OFX_EXTENSIONS_NUKE OFX_EXTENSIONS_TUTTLE OFX_EXTENSIONS_VEGAS OFX_SUPPORTS_PARAMETRIC OFX_EXTENSIONS_TUTTLE OFX_EXTENSIONS_NATRON
DEFINES += OFX_SUPPORTS_MULTITHREAD

trace_ofx_actions{
    DEFINES += OFX_DEBUG_ACTIONS
}

trace_ofx_params{
    DEFINES += OFX_DEBUG_PARAMETERS
}

trace_ofx_properties{
    DEFINES += OFX_DEBUG_PROPERTIES
}

log{
    DEFINES += NATRON_LOG
}

CONFIG(debug, debug|release){
    DEFINES *= DEBUG
} else {
    DEFINES *= NDEBUG
}


CONFIG(noassertions) {
   DEFINES *= NDEBUG QT_NO_DEBUG
}

# https://qt.gitorious.org/qt-creator/qt-creator/commit/b48ba2c25da4d785160df4fd0d69420b99b85152
unix:LIBS += $$QMAKE_LIBS_DYNLOAD

*g++* {
  QMAKE_CXXFLAGS += -ftemplate-depth-1024
  QMAKE_CXXFLAGS_WARN_ON += -Wextra
  GCCVer = $$system($$QMAKE_CXX --version)
  contains(GCCVer,[0-3]\\.[0-9]+.*) {
  } else {
    contains(GCCVer,4\\.7.*) {
      QMAKE_CXXFLAGS += -Wno-c++11-extensions
    }
  }
  c++11 {
    # check for at least version 4.7
    contains(GCCVer,[0-3]\\.[0-9]+.*) {
      error("At least GCC 4.6 is required.")
    } else {
      contains(GCCVer,4\\.[0-5].*) {
        error("At least GCC 4.6 is required.")
      } else {
        contains(GCCVer,4\\.6.*) {
          QMAKE_CXXFLAGS += -std=c++0x
        } else {
          QMAKE_CXXFLAGS += -std=c++11
        }
      }
    }
  }
}

macx {
  # Set the pbuilder version to 46, which corresponds to Xcode >= 3.x
  # (else qmake generates an old pbproj on Snow Leopard)
  QMAKE_PBUILDER_VERSION = 46

  QMAKE_MACOSX_DEPLOYMENT_VERSION = $$split(QMAKE_MACOSX_DEPLOYMENT_TARGET, ".")
  QMAKE_MACOSX_DEPLOYMENT_MAJOR_VERSION = $$first(QMAKE_MACOSX_DEPLOYMENT_VERSION)
  QMAKE_MACOSX_DEPLOYMENT_MINOR_VERSION = $$last(QMAKE_MACOSX_DEPLOYMENT_VERSION)
  universal {
    message("Compiling for universal OSX $${QMAKE_MACOSX_DEPLOYMENT_MAJOR_VERSION}.$$QMAKE_MACOSX_DEPLOYMENT_MINOR_VERSION")
    contains(QMAKE_MACOSX_DEPLOYMENT_MAJOR_VERSION, 10) {
      contains(QMAKE_MACOSX_DEPLOYMENT_TARGET, 4)|contains(QMAKE_MACOSX_DEPLOYMENT_MINOR_VERSION, 5) {
        # OSX 10.4 (Tiger) and 10.5 (Leopard) are x86/ppc
        message("Compiling for universal ppc/i386")
        CONFIG += x86 ppc
      }
      contains(QMAKE_MACOSX_DEPLOYMENT_MINOR_VERSION, 6) {
        message("Compiling for universal i386/x86_64")
        # OSX 10.6 (Snow Leopard) may run on Intel 32 or 64 bits architectures
        CONFIG += x86 x86_64
      }
      # later OSX instances only run on x86_64, universal builds are useless
      # (unless a later OSX supports ARM)
    }
  } 

  #link against the CoreFoundation framework for the StandardPaths functionnality
  LIBS += -framework CoreServices
}

!macx|!universal {
  # precompiled headers don't work with multiple archs
  CONFIG += precompile_header
}

!macx {
  # c++11 build fails on Snow Leopard 10.6 (see the macx section below)
  #CONFIG += c++11
}

win32 {
  #ofx needs WINDOWS def
  #microsoft compiler needs _MBCS to compile with the multi-byte character set.
  DEFINES += WINDOWS _MBCS COMPILED_FROM_DSP XML_STATIC  NOMINMAX
  DEFINES -= _UNICODE UNICODE
  RC_FILE += ../Natron.rc
}

win32-msvc* {

CONFIG(64bit){
	message("Compiling for architecture x86 64 bits")
	Release:DESTDIR = x64/release
	Release:OBJECTS_DIR = x64/release/.obj
	Release:MOC_DIR = x64/release/.moc
	Release:RCC_DIR = x64/release/.rcc
	Release:UI_DIR = x64/release/.ui
	
	Debug:DESTDIR = x64/debug
	Debug:OBJECTS_DIR = x64/debug/.obj
	Debug:MOC_DIR = x64/debug/.moc
	Debug:RCC_DIR = x64/debug/.rcc
	Debug:UI_DIR = x64/debug/.ui
} else {
	message("Compiling for architecture x86 32 bits")
	Release:DESTDIR = win32/release
	Release:OBJECTS_DIR = win32/release/.obj
	Release:MOC_DIR = win32/release/.moc
	Release:RCC_DIR = win32/release/.rcc
	Release:UI_DIR = win32/release/.ui
	
	Debug:DESTDIR = win32/debug
	Debug:OBJECTS_DIR = win32/debug/.obj
	Debug:MOC_DIR = win32/debug/.moc
	Debug:RCC_DIR = win32/debug/.rcc
	Debug:UI_DIR = win32/debug/.ui
}
}

unix {
     #  on Unix systems, only the "boost" option needs to be defined in config.pri
     QT_CONFIG -= no-pkg-config
     CONFIG += link_pkgconfig
     glew:      PKGCONFIG += glew
     expat:     PKGCONFIG += expat
     cairo:     PKGCONFIG += cairo
     !macx {
         LIBS +=  -lGLU
     }
     linux {
         LIBS += -ldl
     }

     # User may specify an alternate python3-config from the command-line,
     # as in "qmake PYTHON_CONFIG=python3.4-config" (MacPorts doesn't have a python3-config)
     isEmpty(PYTHON_CONFIG) {
         PYTHON_CONFIG = python3-config
     }
     #message(PYTHON_CONFIG = $$PYTHON_CONFIG)
     python {
         LIBS += $$system($$PYTHON_CONFIG --ldflags)
         QMAKE_CXXFLAGS += $$system($$PYTHON_CONFIG --includes)
     }

     # There may be different pyside.pc/shiboken.pc for different versions of python.
     # pkg-config will probably give a bad answer, unless python3 is the system default.
     # See for example tools/travis/install_dependencies.sh for a solution that works on Linux,
     # using a custom config.pri
     shiboken: PKGCONFIG += shiboken
     pyside:   PKGCONFIG += pyside
     # The following hack also works with Homebrew if pyside is installed with option --with-python3
     macx {
       shiboken {
         PKGCONFIG -= shiboken
         PYSIDE_PKG_CONFIG_PATH = $$system($$PYTHON_CONFIG --prefix)/lib/pkgconfig
         INCLUDEPATH += $$system(env PKG_CONFIG_PATH=$$PYSIDE_PKG_CONFIG_PATH pkg-config --variable=includedir shiboken)
         # the sed stuff is to work around an Xcode generator bug
         LIBS += $$system(env PKG_CONFIG_PATH=$$PYSIDE_PKG_CONFIG_PATH pkg-config --libs shiboken | sed -e s/-undefined\\ dynamic_lookup//)
       }
       pyside {
         PKGCONFIG -= pyside
         PYSIDE_PKG_CONFIG_PATH = $$system($$PYTHON_CONFIG --prefix)/lib/pkgconfig
         INCLUDEPATH += $$system(env PKG_CONFIG_PATH=$$PYSIDE_PKG_CONFIG_PATH pkg-config --variable=includedir pyside)
         INCLUDEPATH += $$system(env PKG_CONFIG_PATH=$$PYSIDE_PKG_CONFIG_PATH pkg-config --variable=includedir pyside)/QtCore
         # QtGui include are needed because it looks for Qt::convertFromPlainText which is defined in
         # qtextdocument.h in the QtGui module.
         INCLUDEPATH += $$system(env PKG_CONFIG_PATH=$$PYSIDE_PKG_CONFIG_PATH pkg-config --variable=includedir pyside)/QtGui
         INCLUDEPATH += $$system(pkg-config --variable=includedir QtGui)
         LIBS += $$system(env PKG_CONFIG_PATH=$$PYSIDE_PKG_CONFIG_PATH pkg-config --libs pyside)
       }
     }
} #unix

*-xcode {
  # redefine cxx flags as qmake tends to automatically add -O2 to xcode projects
  QMAKE_CFLAGS -= -O2
  QMAKE_CXXFLAGS -= -O2
  QMAKE_CXXFLAGS += -ftemplate-depth-1024
}

*clang* {
  QMAKE_CXXFLAGS += -ftemplate-depth-1024 -Wno-c++11-extensions
  QMAKE_CXXFLAGS_WARN_ON += -Wextra
  c++11 {
    QMAKE_CXXFLAGS += -std=c++11
  }
}

# see http://clang.llvm.org/docs/AddressSanitizer.html and http://blog.qt.digia.com/blog/2013/04/17/using-gccs-4-8-0-address-sanitizer-with-qt/
addresssanitizer {
  message("Compiling with AddressSanitizer (for gcc >= 4.8 and clang). Set the ASAN_SYMBOLIZER_PATH environment variable to point to the llvm-symbolizer binary, or make sure llvm-symbolizer in in your PATH.")
  message("To compile with clang, use a clang-specific spec, such as unsupported/linux-clang, unsupported/macx-clang, linux-clang or macx-clang.")
  message("For example, with Qt4 on OS X:")
  message("  sudo port install clang-3.4")
  message("  sudo port select clang mp-clang-3.4")
  message("  export ASAN_SYMBOLIZER_PATH=/opt/local/bin/llvm-symbolizer-mp-3.4")
  message("  qmake -spec unsupported/macx-clang CONFIG+=addresssanitizer ...")
  message("see http://clang.llvm.org/docs/AddressSanitizer.html")
  CONFIG += debug
  QMAKE_CXXFLAGS += -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls -O1
  QMAKE_CFLAGS += -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls -O1
  QMAKE_LFLAGS += -fsanitize=address -g

#  QMAKE_LFLAGS += -fsanitize-blacklist=../asan_blacklist.ignore
#  QMAKE_CXXFLAGS += -fsanitize-blacklist=../asan_blacklist.ignore
#  QMAKE_CFLAGS += -fsanitize-blacklist=../asan_blacklist.ignore
}

# see http://clang.llvm.org/docs/ThreadSanitizer.html
threadsanitizer {
  message("Compiling with ThreadSanitizer (for clang).")
  message("see http://clang.llvm.org/docs/ThreadSanitizer.html")
  CONFIG += debug
  QMAKE_CXXFLAGS += -fsanitize=thread -O1
  QMAKE_CFLAGS += -fsanitize=thread -O1
  QMAKE_LFLAGS += -fsanitize=thread -g
}

coverage {
  QMAKE_CXXFLAGS += -fprofile-arcs -ftest-coverage -O0
  QMAKE_LFLAGS += -fprofile-arcs -ftest-coverage
  QMAKE_CLEAN += $(OBJECTS_DIR)/*.gcda $(OBJECTS_DIR)/*.gcno
}

