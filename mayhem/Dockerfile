# syntax=docker/dockerfile:1.7
# Commit image for TreeSheets (wxWidgets/GTK3 GUI app, unity build in src/main.cpp).
FROM ghcr.io/savantenvs/base:20261023

ARG COMMIT
ARG REPO_URL
ARG SOURCE_DATE_EPOCH
ARG SANITIZER_FLAGS
ARG COVERAGE_FLAGS=
ARG MAYHEM_JOBS=

# GTK3 for wxGTK, cmake to build wxWidgets, Xvfb so the harness can open a hidden frame.
USER root
RUN [ -n "$SOURCE_DATE_EPOCH" ] && repro-sources-list.sh || true ; \
    apt-get update && apt-get install -yq --no-install-recommends \
        cmake ninja-build pkg-config libgtk-3-dev libsecret-1-dev xvfb xauth fonts-dejavu-core \
 && rm -rf /var/lib/apt/lists/*

# Upstream's CMake FetchContents these; cache them at pinned hashes (SPEC §6.2 item 19).
# wxWidgets is built once, uninstrumented, static, into its own prefix.
ARG WX_VER=3.3.4
ARG WX_SHA256=815c16cafa4fcbfcf25c5c0a3418309c62257b8a3b6f91d85a1367a134f98c98
RUN d=/opt/toolchains/deps/wxwidgets-${WX_VER} && mkdir -p "$d/src" /tmp/wxb \
 && curl -fsSL -o /tmp/wx.tar.bz2 https://github.com/wxWidgets/wxWidgets/releases/download/v${WX_VER}/wxWidgets-${WX_VER}.tar.bz2 \
 && echo "${WX_SHA256}  /tmp/wx.tar.bz2" | sha256sum -c - \
 && tar -xjf /tmp/wx.tar.bz2 --strip-components=1 -C "$d/src" && rm /tmp/wx.tar.bz2 \
 && cd /tmp/wxb && cmake -G Ninja "$d/src" -DCMAKE_INSTALL_PREFIX="$d" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-O1 -g -gdwarf-3" -DCMAKE_CXX_FLAGS="-O1 -g -gdwarf-3" \
      -DwxBUILD_SHARED=OFF -DwxBUILD_SAMPLES=OFF -DwxBUILD_TESTS=OFF -DwxBUILD_DEMOS=OFF \
      -DwxUSE_WEBVIEW=OFF -DwxUSE_STC=OFF -DwxUSE_RIBBON=OFF -DwxUSE_PROPGRID=OFF -DwxUSE_RICHTEXT=OFF \
      -DwxUSE_MEDIACTRL=OFF -DwxUSE_HTML=OFF -DwxUSE_WXHTML_HELP=OFF -DwxUSE_OPENGL=OFF \
      -DwxUSE_WEBREQUEST=OFF -DwxUSE_LIBSDL=OFF -DwxUSE_LIBNOTIFY=OFF -DwxUSE_LIBMSPACK=OFF \
      -DwxUSE_GLCANVAS=OFF -DwxUSE_SECRETSTORE=OFF \
 && ninja -j"$(nproc)" install && cd / && rm -rf /tmp/wxb && chmod -R a+rX "$d"
ARG MONOCYPHER_SHA256=a7cbae546fbdc489bca632c3747e1ceb8ca3d4bd39e2706a0916f28ccd280e50
ARG UNIBREAK_SHA256=9c4fad6e517338a098373acc9f35579ae2c325e6446666fb9ac2666ba15ceba4
RUN d=/opt/toolchains/deps/monocypher-4.0.3 && mkdir -p "$d/src" \
 && curl -fsSL -o /tmp/m.tgz https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.3.tar.gz \
 && echo "${MONOCYPHER_SHA256}  /tmp/m.tgz" | sha256sum -c - && tar -xzf /tmp/m.tgz --strip-components=1 -C "$d/src" \
 && mkdir -p "$d/include" "$d/lib" && cp "$d/src/src/monocypher.h" "$d/include/" \
 && clang -O1 -g -gdwarf-3 -c "$d/src/src/monocypher.c" -o /tmp/monocypher.o && ar rcs "$d/lib/libmonocypher.a" /tmp/monocypher.o \
 && u=/opt/toolchains/deps/libunibreak-8.0 && mkdir -p "$u/src" "$u/include" "$u/lib" \
 && curl -fsSL -o /tmp/u.tgz https://github.com/adah1972/libunibreak/releases/download/libunibreak_8_0/libunibreak-8.0.tar.gz \
 && echo "${UNIBREAK_SHA256}  /tmp/u.tgz" | sha256sum -c - && tar -xzf /tmp/u.tgz --strip-components=1 -C "$u/src" \
 && cd /tmp && for f in unibreakbase unibreakdef linebreak linebreakdata linebreakdef eastasianwidthdef emojidef; do \
      clang -O1 -g -gdwarf-3 -c "$u/src/src/$f.c" -o /tmp/ub_$f.o; done \
 && ar rcs "$u/lib/libunibreak.a" /tmp/ub_*.o && cp "$u/src/src/"*.h "$u/include/" \
 && rm -f /tmp/*.o /tmp/*.tgz && chmod -R a+rX "$d" "$u"

# Lobster (the script engine, with its libtcc JIT), as upstream's cmake/Lobster.cmake builds it.
ARG LOBSTER_VER=2026.9
ARG LOBSTER_SHA256=4f1de38fb65faf654092c407b2420411a6bfae0cdce186a97844e5f200a399ac
RUN d=/opt/toolchains/deps/lobster-${LOBSTER_VER} && mkdir -p "$d/src" "$d/lib" /tmp/lb \
 && curl -fsSL -o /tmp/lobster.tgz https://github.com/aardappel/lobster/archive/refs/tags/v${LOBSTER_VER}.tar.gz \
 && echo "${LOBSTER_SHA256}  /tmp/lobster.tgz" | sha256sum -c - \
 && tar -xzf /tmp/lobster.tgz --strip-components=1 -C "$d/src" && rm /tmp/lobster.tgz \
 && cd "$d/src/dev" && inc="-Isrc -Iinclude -Iexternal -Iexternal/libtcc" \
 && for f in external/flatbuffers/src/idl_gen_text.cpp external/flatbuffers/src/idl_parser.cpp \
       external/flatbuffers/src/util.cpp src/builtindoc.cpp src/builtins.cpp src/compiler.cpp src/file.cpp \
       src/tcp.cpp src/pakfile.cpp src/lobsterreader.cpp src/platform.cpp src/simplex.cpp src/vm.cpp \
       src/vmdata.cpp src/tccbind.cpp; do \
      clang++ -std=c++20 -O1 -g -gdwarf-3 -DLOBSTER_ENGINE=0 $inc -c "$f" -o "/tmp/lb/$(basename "$f").o" & done; wait \
 && clang -O1 -g -gdwarf-3 -DLOBSTER_ENGINE=0 $inc -c external/libtcc/libtcc.c -o /tmp/lb/libtcc.o \
 && ls /tmp/lb/*.o | wc -l | grep -qx 16 \
 && ar rcs "$d/lib/liblobster.a" /tmp/lb/*.o && rm -rf /tmp/lb && chmod -R a+rX "$d"

USER mayhem
WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

RUN if [ -x /opt/mayhem/ccache/prefill ]; then /opt/mayhem/ccache/prefill bash mayhem/build.sh; \
    else bash mayhem/build.sh; fi

RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.source_date_epoch="${SOURCE_DATE_EPOCH}" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem
